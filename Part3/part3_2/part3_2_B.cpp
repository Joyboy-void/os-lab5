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
#include "../includes/rowPacket.h"
#include "../includes/libppm.h"


const bool USE_HASH = true;
const int PROCESSED_ROW_COUNT = 32;
const int SCALING_FACTOR = 2;

// header formate: int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last
static const size_t HDR_SIZE = sizeof(int32_t)*3 + sizeof(uint64_t) + sizeof(uint8_t);

// named shared memory & semaphores 
static const char* SHM_S2_S3_NAME = "/shm_s2_s3";

static const char* SEM_S2S3_EMPTY = "/sem_s2s3_empty";
static const char* SEM_S2S3_FULL  = "/sem_s2s3_full";

// inherited by children
static size_t g_cols_per_row = 0;
static size_t g_fixed_payload = 0;  // = PROCESSED_ROW_COUNT * cols_per_row * 3
static size_t g_shm_size = 0;       // HDR_SIZE + fixed_payload

static char* shm_s2_s3 = nullptr;

static sem_t* sem_s2s3_empty = nullptr;
static sem_t* sem_s2s3_full  = nullptr;

// TCP socket for S2
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

// write a block into a shared buffer using semaphores
static void write_shm_block(char* shm_ptr, sem_t* sem_empty, sem_t* sem_full, const std::vector<char>& buf) {
    // wait for empty slot
    if (sem_wait(sem_empty) == -1) {
        perror("sem_wait empty");
        _exit(1);
    }
    // copy entire buffer into shared memory of size shm_size
    memcpy(shm_ptr, buf.data(), g_shm_size);

    // increment full
    if (sem_post(sem_full) == -1) {
        perror("sem_post full");
        _exit(1);
    }
}

// read a block from shared buffer into local buffer
static void read_shm_block(char* shm_ptr, sem_t* sem_empty, sem_t* sem_full, std::vector<char>& outbuf) {
    if (sem_wait(sem_full) == -1) {
        perror("sem_wait full");
        _exit(1);
    }
    memcpy(outbuf.data(), shm_ptr, g_shm_size);

    // increment empty
    if (sem_post(sem_empty) == -1) {
        perror("sem_post empty");
        _exit(1);
    }
}


void S2_find_details(image_t* input_image) {
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        // forward terminal
        std::vector<char> termbuf(g_shm_size);
        char hdr[HDR_SIZE];
        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        memcpy(termbuf.data(), hdr, HDR_SIZE);

        write_shm_block(shm_s2_s3, sem_s2s3_empty, sem_s2s3_full, termbuf);

        return;
    }

    const int cols_per_row = std::max(0, width - 2);

    std::vector<char> hdrbuf(g_shm_size);

    while (true) {
        recv_all(g_sock, hdrbuf.data(), g_shm_size);

        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        deserialize_header(hdrbuf.data(), start_row, num_rows, cols, hash, is_last);

        // payload is part of hdrbuf (readed full block already)
        if (is_last) {
            // forward terminal header

            std::vector<char> termbuf(g_shm_size);
            char thdr[HDR_SIZE];
            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            memcpy(termbuf.data(), thdr, HDR_SIZE);

            write_shm_block(shm_s2_s3, sem_s2s3_empty, sem_s2s3_full, termbuf);
            return;
        }

        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual_bytes = static_cast<size_t>(num_rows) * cols * 3;
        if (actual_bytes > 0)
            memcpy(rpkt.pixels.data(), hdrbuf.data() + HDR_SIZE, actual_bytes);

        rpkt.hash = (std::size_t)hash;

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S2: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";

                std::vector<char> termbuf(g_shm_size);
                char thdr[HDR_SIZE];
                serialize_header(thdr, -1, 0, 0, 0ULL, 1);
                memcpy(termbuf.data(), thdr, HDR_SIZE);

                write_shm_block(shm_s2_s3, sem_s2s3_empty, sem_s2s3_full, termbuf);

                return;
            }
        }

        rowPacket out_rpkt(rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row);

        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int row_idx = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {

                uint8_t* sp = rpkt.pixel_ptr(r_off, cidx);
                int orig_col = 1 + cidx;
                
                int dR = input_image->image_pixels[row_idx][orig_col][0] - (int)sp[0];
                int dG = input_image->image_pixels[row_idx][orig_col][1] - (int)sp[1];
                int dB = input_image->image_pixels[row_idx][orig_col][2] - (int)sp[2];
                
                if (dR < 0) 
                    dR = 0;
                if (dG < 0) 
                    dG = 0; 
                if (dB < 0) 
                    dB = 0;

                uint8_t* op = out_rpkt.pixel_ptr(r_off, cidx);

                op[0] = (uint8_t)dR; op[1] = (uint8_t)dG; op[2] = (uint8_t)dB;
            }
        }

        if (USE_HASH) out_rpkt.hash = calculate_hash_for_packet(out_rpkt);

        std::vector<char> outbuf(g_shm_size);
        serialize_header(outbuf.data(), out_rpkt.start_row, out_rpkt.num_rows, out_rpkt.cols_per_row, (uint64_t)out_rpkt.hash, out_rpkt.is_last ? 1 : 0);
        size_t out_actual = static_cast<size_t>(out_rpkt.num_rows) * out_rpkt.cols_per_row * 3;

        if (out_actual > 0) 
            memcpy(outbuf.data() + HDR_SIZE, out_rpkt.pixels.data(), out_actual);
        if (g_shm_size > HDR_SIZE + out_actual) 
            memset(outbuf.data() + HDR_SIZE + out_actual, 0, g_shm_size - HDR_SIZE - out_actual);

        write_shm_block(shm_s2_s3, sem_s2s3_empty, sem_s2s3_full, outbuf);
    }
}

void S3_sharpen(image_t* input_image, image_t* output_image) {
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        std::cerr << "Immage too small" << std::endl;
        return;
    }

    std::vector<char> blockbuf(g_shm_size);

    while (true) {

        read_shm_block(shm_s2_s3, sem_s2s3_empty, sem_s2s3_full, blockbuf);

        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        deserialize_header(blockbuf.data(), start_row, num_rows, cols, hash, is_last);

        if (is_last) {
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
                _exit(1);
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

// helper to create shared mem and map of size bytes, return pointer
static char* create_and_map_shm(const char* name, size_t size) {
    
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { 
        perror("shm_open"); 
        return nullptr; 
    }

    if (ftruncate(fd, (off_t)size) == -1) {
        perror("ftruncate"); 
        close(fd); 
        return nullptr; 
    }

    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (p == MAP_FAILED) { 
        perror("mmap"); 
        close(fd); 
        return nullptr; 
    }

    close(fd);
    return static_cast<char*>(p);
}

int main(int argc, char **argv) {

    // usage: ./b.out <input.ppm> <output.ppm> [server_ip] [port]
    if (argc != 3 && argc != 5) {
        std::cout << "usage: ./b.out <input.ppm> <output.ppm> [server_ip] [port]\n";
        return 0;
    }

    image_t* input_image = read_ppm_file(argv[1]);
    if (!input_image) { 
        std::cerr << "Failed to read input\n"; 
        return 1; 
    }

    const char* server_ip = (argc == 5) ? argv[3] : "127.0.0.1";

    int server_port = (argc == 5) ? std::atoi(argv[4]) : 9090;

    int height = input_image->height, width = input_image->width;

    // allocate space for output_image
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

    // set global varibales

    g_cols_per_row = static_cast<size_t>(std::max(0, width - 2));
    g_fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * g_cols_per_row * 3;
    g_shm_size = HDR_SIZE + g_fixed_payload;

    // create shared memory regions using helper
    shm_s2_s3 = create_and_map_shm(SHM_S2_S3_NAME, g_shm_size);

    // server connection
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

    std::cout << "S2_S3: Connected to S1 at " << server_ip << ":" << server_port << "\n";

    if (!shm_s2_s3 ) { 
        std::cerr << "Failed to create shared memory\n"; 
        return 1; 
    }

    // create semaphores (initially empty=1, full=0)
    sem_unlink(SEM_S2S3_EMPTY); sem_unlink(SEM_S2S3_FULL);

    // create semaphores
    sem_s2s3_empty = sem_open(SEM_S2S3_EMPTY, O_CREAT, 0666, 1);
    sem_s2s3_full  = sem_open(SEM_S2S3_FULL,  O_CREAT, 0666, 0);

    if (sem_s2s3_empty == SEM_FAILED || sem_s2s3_full == SEM_FAILED) {
        perror("sem_open"); return 1;
    }

    auto start_p = std::chrono::steady_clock::now();

    
   
    // fork S2
    pid_t pid2 = fork();

    if (pid2 < 0) { 
        perror("fork2"); 
        exit(1); 
    }
    if (pid2 == 0) {
        S2_find_details(input_image);

        //cleanup
        munmap(shm_s2_s3, g_shm_size);
        sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);

        _exit(0);
    }

    // fork S3
    pid_t pid3 = fork();

    if (pid3 < 0) { 
        perror("fork3"); 
        exit(1); 
    }
    if (pid3 == 0) {
        S3_sharpen(input_image,output_image);

        //cleanup
        munmap(shm_s2_s3, g_shm_size);
        sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);

        _exit(0);
    }

    
    munmap(shm_s2_s3, g_shm_size);

    waitpid(pid2, nullptr, 0);
    waitpid(pid3, nullptr, 0);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;
    std::cout << "Total Processing time : " << elapsed.count()*1000 << " ms\n";

    write_ppm_file(argv[2], output_image);
    std::cout << "Image written to " << argv[2] << std::endl;

    // unlink and close semaphores 
    sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);
    sem_unlink(SEM_S2S3_EMPTY); sem_unlink(SEM_S2S3_FULL);
    shm_unlink(SHM_S2_S3_NAME);

    return 0;
}