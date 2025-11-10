#include <iostream>
#include <cstdint>
#include <cstddef>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../include/rowPacket.h"
#include "../../include/libppm.h"   

const bool USE_HASH = true;
const int PROCESSED_ROW_COUNT = 32;
const int SCALING_FACTOR = 2;

// header formate: int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last
static const size_t HDR_SIZE = sizeof(int32_t)*3 + sizeof(uint64_t) + sizeof(uint8_t);

// inherited by children
static size_t g_cols_per_row = 0;
static size_t g_fixed_payload = 0;  // = PROCESSED_ROW_COUNT * cols_per_row * 3
static size_t g_shm_size = 0;       // HDR_SIZE + fixed_payload

// TCP socket for S3
static int g_sock = -1;

static bool recv_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining) {
        ssize_t n = ::recv(fd, p, remaining, 0);
        if (n == 0) {
            // connection closed
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

static void serialize_header(char *dst, int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last) {
    size_t off = 0;
    memcpy(dst + off, &start_row, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(dst + off, &num_rows, sizeof(int32_t));  off += sizeof(int32_t);
    memcpy(dst + off, &cols_per_row, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(dst + off, &hash, sizeof(uint64_t)); off += sizeof(uint64_t);
    memcpy(dst + off, &is_last, sizeof(uint8_t)); off += sizeof(uint8_t);
    (void)off;
}

static void deserialize_header(const char *src, int32_t &start_row, int32_t &num_rows, int32_t &cols_per_row, uint64_t &hash, uint8_t &is_last) {
    size_t off = 0;
    memcpy(&start_row, src + off, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(&num_rows, src + off, sizeof(int32_t));  off += sizeof(int32_t);
    memcpy(&cols_per_row, src + off, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(&hash, src + off, sizeof(uint64_t)); off += sizeof(uint64_t);
    memcpy(&is_last, src + off, sizeof(uint8_t)); off += sizeof(uint8_t);
    (void)off;
}

// FNV hash generating function
static std::size_t calculate_hash_for_packet(const rowPacket &rp) {
    if (rp.pixels.empty()) 
        return 0;
    const std::size_t FNV_offset = 1469598103934665603ULL;
    const std::size_t FNV_prime  = 1099511628211ULL;
    std::size_t h = FNV_offset;

    for (uint8_t b : rp.pixels){
        h ^= static_cast<std::size_t>(b);
        h *= FNV_prime;
    }

    h ^= static_cast<std::size_t>(rp.start_row + 0x9e3779b9);
    h *= FNV_prime;
    h ^= static_cast<std::size_t>(rp.num_rows + 0x9e3779b9);
    h *= FNV_prime;

    return h;
}

void S3_sharpen(image_t* input_image,image_t* output_image) {
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        _exit(1);
    }

    std::vector<char> blockbuf(g_shm_size);

    while (true) {
        //  read a fixed-size block
        if (!recv_all(g_sock, blockbuf.data(), g_shm_size)) {
            std::cerr << "S3: connection closed/failed\n";
            return;
        }

        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;

        deserialize_header(blockbuf.data(), start_row, num_rows, cols, hash, is_last);

        if (is_last) {
            // terminal marker from A
            return;
        }

        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual = static_cast<size_t>(num_rows) * cols * 3;
        if (actual > 0) 
            memcpy(rpkt.pixels.data(), blockbuf.data() + HDR_SIZE, actual);

        rpkt.hash = (std::size_t)hash;

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S3: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";
                return;
            }
        }

    
        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int i = rpkt.start_row + r_off;

            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {
                uint8_t* d = rpkt.pixel_ptr(r_off, cidx);
                int j = 1 + cidx;
                
                int newR = input_image->image_pixels[i][j][0] + SCALING_FACTOR * (int)d[0];
                int newG = input_image->image_pixels[i][j][1] + SCALING_FACTOR * (int)d[1];
                int newB = input_image->image_pixels[i][j][2] + SCALING_FACTOR * (int)d[2];

                output_image->image_pixels[i][j][0] = (uint8_t)(newR > 255 ? 255 : (newR < 0 ? 0 : newR));
                output_image->image_pixels[i][j][1] = (uint8_t)(newG > 255 ? 255 : (newG < 0 ? 0 : newG));
                output_image->image_pixels[i][j][2] = (uint8_t)(newB > 255 ? 255 : (newB < 0 ? 0 : newB));
            }
        }
    }
}

int main(int argc, char **argv) {
    // usage: ./b.out <input.ppm> <output.ppm> [server_ip] [port]
    if (argc != 3 && argc != 5) {
        std::cout << "usage: ./b.out <input.ppm> <output.ppm> [server_ip] [port]\n";
        return 0;
    }

    std::cout << "\nProcessing S3 and Writing Image..." <<std::endl;
    std::cout << "----------------------------------------------------------------------------------------------------------" << std::endl;

    const char* server_ip = (argc == 5) ? argv[3] : "127.0.0.1";

    int server_port = (argc == 5) ? std::atoi(argv[4]) : 9090;

    image_t* input_image = read_ppm_file(argv[1]);
    if (!input_image) { 
        std::cerr << "Failed to read input\n"; 
        return 1; 
    }

    int height = input_image->height, width = input_image->width;

    // initilize output_image
    image_t* output_image = new image_t;

    output_image->height = height; output_image->width = width;
    output_image->image_pixels = new uint8_t**[height];
    
    for (int i = 0; i < height; ++i) {
        output_image->image_pixels[i] = new uint8_t*[width];
        for (int j = 0; j < width; ++j) {
            output_image->image_pixels[i][j] = new uint8_t[3];
    
            output_image->image_pixels[i][j][0] = input_image->image_pixels[i][j][0];
            output_image->image_pixels[i][j][1] = input_image->image_pixels[i][j][1];
            output_image->image_pixels[i][j][2] = input_image->image_pixels[i][j][2];
        }
    }

    g_cols_per_row = static_cast<size_t>(std::max(0, width - 2));
    g_fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * g_cols_per_row * 3;
    g_shm_size = HDR_SIZE + g_fixed_payload;


    g_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { 
        perror("socket"); 
        return 1; 
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &sa.sin_addr) <= 0) {
        perror("inet_pton");
        return 1;
    }

    if (connect(g_sock, (sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("connect");
        return 1;
    }

    std::cout << "B: Connected to A at " << server_ip << ":" << server_port << "\n";

    auto start_p = std::chrono::steady_clock::now();
    
    S3_sharpen(input_image, output_image);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;

    std::cout << "Total Processing time : " << elapsed.count()*1000 << " ms\n";

    write_ppm_file(argv[2], output_image);
    std::cout << "Image written to " << argv[2] << std::endl;

    if (g_sock >= 0) close(g_sock);
    return 0;
}
