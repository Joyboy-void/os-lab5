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


static size_t g_cols_per_row = 0;
static size_t g_fixed_payload = 0;  // = PROCESSED_ROW_COUNT * cols_per_row * 3
static size_t g_shm_size = 0;       // HDR_SIZE + fixed_payload

// global accepted socket for S2
static int g_client_fd = -1;

// helper to send buffer

static bool send_all(int fd, const void* buf, size_t len) {

    const char* p = static_cast<const char*>(buf);
    while (len) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) 
                continue;
            perror("send");
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
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

void S1_smoothen(image_t* input_image) {
    
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        // write terminal into S1_S2
        std::vector<char> termbuf(g_shm_size);
        char hdr[HDR_SIZE];

        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        memcpy(termbuf.data(), hdr, HDR_SIZE);

        send_all(g_client_fd, termbuf.data(),g_shm_size);
        
        return;
    }

    const int cols_per_row = std::max(0, width - 2);
    const size_t fixed_payload = g_fixed_payload; // PROCESSED_ROW_COUNT * cols_per_row * 3

    int dir[9][2] = {
        {-1,-1}, {-1,0}, {-1,1},
        {0,-1}, {0,0}, {0,1},
        {1,-1}, {1,0}, {1,1}
    };

    for (int i = 1; i <= height - 2; ) {
        int batch_start = i;
        int take = std::min(PROCESSED_ROW_COUNT, (height - 1) - i );

        if (take <= 0) 
            break;

        rowPacket rpkt(batch_start, take, cols_per_row);

        for (int r_off = 0; r_off < take; ++r_off) {
            int r = batch_start + r_off;
            for (int c = 1, cidx = 0; c <= width - 2; c++, cidx++) {
                int sumR=0, sumG=0, sumB=0;
                for (int k=0;k<9;++k) {

                    int rr = r + dir[k][0], cc = c + dir[k][1];

                    sumR += input_image->image_pixels[rr][cc][0];
                    sumG += input_image->image_pixels[rr][cc][1];
                    sumB += input_image->image_pixels[rr][cc][2];
                }

                uint8_t sr = static_cast<uint8_t>(sumR / 9);
                uint8_t sg = static_cast<uint8_t>(sumG / 9);
                uint8_t sb = static_cast<uint8_t>(sumB / 9);
                
                uint8_t *p = rpkt.pixel_ptr(r_off, cidx);
                
                p[0]=sr; p[1]=sg; p[2]=sb;
            }
        }

        if (USE_HASH)
            rpkt.hash = calculate_hash_for_packet(rpkt);

        std::vector<char> outbuf(g_shm_size);
        serialize_header(outbuf.data(), rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row, (uint64_t)rpkt.hash, rpkt.is_last ? 1 : 0);

        size_t actual_bytes = static_cast<size_t>(take) * cols_per_row * 3;

        if (actual_bytes > 0) 
            memcpy(outbuf.data() + HDR_SIZE, rpkt.pixels.data(), actual_bytes);

        if (g_shm_size > HDR_SIZE + actual_bytes) 
            memset(outbuf.data() + HDR_SIZE + actual_bytes, 0, g_shm_size - HDR_SIZE - actual_bytes);

        send_all(g_client_fd,outbuf.data(),g_shm_size);

        i += take;
    }

    // send terminal 
    std::vector<char> termbuf(g_shm_size);
    char thdr[HDR_SIZE];
    serialize_header(thdr, -1, 0, 0, 0ULL, 1);
    memcpy(termbuf.data(), thdr, HDR_SIZE);

    send_all(g_client_fd,termbuf.data(),g_shm_size);
}


int main(int argc, char **argv) {

   // usage: ./a.out <input.ppm> [port]
    // specifing port is optional
    if (argc != 2 && argc != 3) {
        std::cout << "usage: ./a.out <input.ppm> [port]\n";
        return 0;
    }

    std::cout << "\nProcessing S1... " <<std::endl;
    std::cout << "----------------------------------------------------------------------------------------------------------" << std::endl;

    int listen_port = (argc == 3) ? std::atoi(argv[2]) : 9090;

    image_t* input_image = read_ppm_file(argv[1]);
    if (!input_image) { 
        std::cerr << "Failed to read input\n"; 
        return 1; 
    }

    int height = input_image->height, width = input_image->width;

    // set global varibales

    g_cols_per_row = static_cast<size_t>(std::max(0, width - 2));
    g_fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * g_cols_per_row * 3;
    g_shm_size = HDR_SIZE + g_fixed_payload;

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { 
        perror("socket"); 
        return 1; 
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { 
        perror("bind"); 
        return 1; 
    }
    if (listen(server_fd, 1) < 0) { 
        perror("listen"); 
        return 1; 
    }

    std::cout << "S1: Listening on port " << listen_port << " for S2_S3...\n";

    sockaddr_in cli{};
    socklen_t cl = sizeof(cli);
    g_client_fd = accept(server_fd, (sockaddr*)&cli, &cl);
    
    if (g_client_fd < 0) { 
        perror("accept"); 
        return 1; 
    }

    std::cout << "A1: S2_S3 connected.\n";

    auto start_p = std::chrono::steady_clock::now();

    S1_smoothen(input_image);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;
    std::cout << "Total Processing time per iteration " << elapsed.count()*1000 << " ms\n";

    return 0;
}
