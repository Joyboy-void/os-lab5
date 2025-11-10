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

#include "../../include/rowPacket.h"
#include "../../include/libppm.h"   


const bool USE_HASH = true;
const int MAX_ITERATIONS = 1;
const int PROCESSED_ROW_COUNT = 32;
const int SCALING_FACTOR = 2;

// header formate: int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last
static const size_t HDR_SIZE = sizeof(int32_t)*3 + sizeof(uint64_t) + sizeof(uint8_t);

// named shared memory & semaphores 
static const char* SHM_S1_S2_NAME = "/shm_s1_s2";
static const char* SHM_S2_S3_NAME = "/shm_s2_s3";
static const char* SHM_S3_P_NAME  = "/shm_s3_p";

static const char* SEM_S1S2_EMPTY = "/sem_s1s2_empty";
static const char* SEM_S1S2_FULL  = "/sem_s1s2_full";
static const char* SEM_S2S3_EMPTY = "/sem_s2s3_empty";
static const char* SEM_S2S3_FULL  = "/sem_s2s3_full";
static const char* SEM_S3P_EMPTY  = "/sem_s3p_empty";
static const char* SEM_S3P_FULL   = "/sem_s3p_full";

// inherited by children
static size_t g_cols_per_row = 0;
static size_t g_fixed_payload = 0;  // = PROCESSED_ROW_COUNT * cols_per_row * 3
static size_t g_shm_size = 0;       // HDR_SIZE + fixed_payload

static char* shm_s1_s2 = nullptr;
static char* shm_s2_s3 = nullptr;
static char* shm_s3_p  = nullptr;

static sem_t* sem_s1s2_empty = nullptr;
static sem_t* sem_s1s2_full  = nullptr;
static sem_t* sem_s2s3_empty = nullptr;
static sem_t* sem_s2s3_full  = nullptr;
static sem_t* sem_s3p_empty  = nullptr;
static sem_t* sem_s3p_full   = nullptr;

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

void S1_smoothen(image_t* input_image) {
    
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        // write terminal into S1_S2
        std::vector<char> termbuf(g_shm_size);
        char hdr[HDR_SIZE];

        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        memcpy(termbuf.data(), hdr, HDR_SIZE);

        write_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, termbuf);
        
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

        write_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, outbuf);

        i += take;
    }

    // send terminal 
    std::vector<char> termbuf(g_shm_size);
    char thdr[HDR_SIZE];
    serialize_header(thdr, -1, 0, 0, 0ULL, 1);
    memcpy(termbuf.data(), thdr, HDR_SIZE);

    write_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, termbuf);
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
        read_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, hdrbuf);

        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        deserialize_header(hdrbuf.data(), start_row, num_rows, cols, hash, is_last);

        // payload is part of hdrbuf (readed full block already)
        if (is_last) {
            // forward terminal header + 0- payload
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

void S3_sharpen(image_t* input_image) {
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        // forward terminal
        std::vector<char> termbuf(g_shm_size);
        char hdr[HDR_SIZE];
        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        memcpy(termbuf.data(), hdr, HDR_SIZE);

        write_shm_block(shm_s3_p, sem_s3p_empty, sem_s3p_full, termbuf);
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
            std::vector<char> termbuf(g_shm_size);
            char thdr[HDR_SIZE];
            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            memcpy(termbuf.data(), thdr, HDR_SIZE);

            write_shm_block(shm_s3_p, sem_s3p_empty, sem_s3p_full, termbuf);

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

                std::vector<char> termbuf(g_shm_size);
                char thdr[HDR_SIZE];
                serialize_header(thdr, -1, 0, 0, 0ULL, 1);
                memcpy(termbuf.data(), thdr, HDR_SIZE);

                write_shm_block(shm_s3_p, sem_s3p_empty, sem_s3p_full, termbuf);

                return;
            }
        }

        rowPacket out_rpkt(rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row);

        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int i = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {

                uint8_t* d = rpkt.pixel_ptr(r_off, cidx);
                int j = 1 + cidx;
                
                int newR = input_image->image_pixels[i][j][0] + SCALING_FACTOR * (int)d[0];
                int newG = input_image->image_pixels[i][j][1] + SCALING_FACTOR * (int)d[1];
                int newB = input_image->image_pixels[i][j][2] + SCALING_FACTOR * (int)d[2];
                
                uint8_t* o = out_rpkt.pixel_ptr(r_off, cidx);
                
                o[0] = (uint8_t)(newR > 255 ? 255 : (newR < 0 ? 0 : newR));
                o[1] = (uint8_t)(newG > 255 ? 255 : (newG < 0 ? 0 : newG));
                o[2] = (uint8_t)(newB > 255 ? 255 : (newB < 0 ? 0 : newB));
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

        write_shm_block(shm_s3_p, sem_s3p_empty, sem_s3p_full, outbuf);
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
    if (argc != 3) {
        std::cout << "usage: ./a.out <input.ppm> <output.ppm>\n";
        return 0;
    }

    std::cout << "\nProcessing Image..." <<std::endl;
    std::cout << "----------------------------------------------------------------------------------------------------------" << std::endl;

    image_t* input_image = read_ppm_file(argv[1]);
    if (!input_image) { std::cerr << "Failed to read input\n"; return 1; }

    int height = input_image->height, width = input_image->width;

    // prepare output image 
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
    shm_s1_s2 = create_and_map_shm(SHM_S1_S2_NAME, g_shm_size);
    shm_s2_s3 = create_and_map_shm(SHM_S2_S3_NAME, g_shm_size);
    shm_s3_p  = create_and_map_shm(SHM_S3_P_NAME,  g_shm_size);
    if (!shm_s1_s2 || !shm_s2_s3 || !shm_s3_p) { std::cerr << "Failed to create shared memory\n"; return 1; }

    // create semaphores (initially empty=1, full=0)
    sem_unlink(SEM_S1S2_EMPTY); sem_unlink(SEM_S1S2_FULL);
    sem_unlink(SEM_S2S3_EMPTY); sem_unlink(SEM_S2S3_FULL);
    sem_unlink(SEM_S3P_EMPTY);  sem_unlink(SEM_S3P_FULL);

    // create semaphores
    sem_s1s2_empty = sem_open(SEM_S1S2_EMPTY, O_CREAT, 0666, 1);
    sem_s1s2_full  = sem_open(SEM_S1S2_FULL,  O_CREAT, 0666, 0);
    sem_s2s3_empty = sem_open(SEM_S2S3_EMPTY, O_CREAT, 0666, 1);
    sem_s2s3_full  = sem_open(SEM_S2S3_FULL,  O_CREAT, 0666, 0);
    sem_s3p_empty  = sem_open(SEM_S3P_EMPTY,  O_CREAT, 0666, 1);
    sem_s3p_full   = sem_open(SEM_S3P_FULL,   O_CREAT, 0666, 0);

    if (sem_s1s2_empty == SEM_FAILED || sem_s1s2_full == SEM_FAILED || sem_s2s3_empty == SEM_FAILED || sem_s2s3_full == SEM_FAILED || sem_s3p_empty  == SEM_FAILED || sem_s3p_full  == SEM_FAILED) {
        perror("sem_open"); return 1;
    }

    auto start_p = std::chrono::steady_clock::now();

    
    pid_t pid1 = fork();

    if (pid1 < 0) { 
        perror("fork1"); 
        exit(1); 
    }
    if (pid1 == 0) {
        S1_smoothen(input_image);
        
        // cleanup
        munmap(shm_s1_s2, g_shm_size);
        munmap(shm_s2_s3, g_shm_size);
        munmap(shm_s3_p,  g_shm_size);
        sem_close(sem_s1s2_empty); sem_close(sem_s1s2_full);
        sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);
        sem_close(sem_s3p_empty);  sem_close(sem_s3p_full);
        _exit(0);
    }

    // fork S2
    pid_t pid2 = fork();

    if (pid2 < 0) { 
        perror("fork2"); 
        exit(1); 
    }
    if (pid2 == 0) {
        S2_find_details(input_image);

        //cleanup
        munmap(shm_s1_s2, g_shm_size);
        munmap(shm_s2_s3, g_shm_size);
        munmap(shm_s3_p,  g_shm_size);
        sem_close(sem_s1s2_empty); sem_close(sem_s1s2_full);
        sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);
        sem_close(sem_s3p_empty);  sem_close(sem_s3p_full);
        _exit(0);
    }

    // fork S3
    pid_t pid3 = fork();

    if (pid3 < 0) { 
        perror("fork3"); 
        exit(1); 
    }
    if (pid3 == 0) {
        S3_sharpen(input_image);

        //cleanup
        munmap(shm_s1_s2, g_shm_size);
        munmap(shm_s2_s3, g_shm_size);
        munmap(shm_s3_p,  g_shm_size);
        sem_close(sem_s1s2_empty); sem_close(sem_s1s2_full);
        sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);
        sem_close(sem_s3p_empty);  sem_close(sem_s3p_full);
        _exit(0);
    }

    // parent: read from shm_s3_p and write to output_image
    std::vector<char> blockbuf(g_shm_size);

    while (true) {
        read_shm_block(shm_s3_p, sem_s3p_empty, sem_s3p_full, blockbuf);

        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        
        deserialize_header(blockbuf.data(), start_row, num_rows, cols, hash, is_last);

        if (is_last) 
            break;

        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual = static_cast<size_t>(num_rows) * cols * 3;

        if (actual > 0) 
            memcpy(rpkt.pixels.data(), blockbuf.data() + HDR_SIZE, actual);

        if (USE_HASH) {
            if (calculate_hash_for_packet(rpkt) != (std::size_t)hash) {
                std::cerr << "Parent: data corrupted for row " << rpkt.start_row << "\n";
                break;
            }
        }

        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int r = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {
                int j = 1 + cidx;

                uint8_t* src = rpkt.pixel_ptr(r_off, cidx);
                
                output_image->image_pixels[r][j][0] = src[0];
                output_image->image_pixels[r][j][1] = src[1];
                output_image->image_pixels[r][j][2] = src[2];
            }
        }
    }


    munmap(shm_s1_s2, g_shm_size);
    munmap(shm_s2_s3, g_shm_size);
    munmap(shm_s3_p,  g_shm_size);

    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);
    waitpid(pid3, nullptr, 0);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;

    std::cout << "Total Processing time per iteration " << elapsed.count()*1000/MAX_ITERATIONS << " ms\n";

    write_ppm_file(argv[2], output_image);
    std::cout << "Image written to " << argv[2] << std::endl;

    // unlink and close semaphores 
    sem_close(sem_s1s2_empty); sem_close(sem_s1s2_full);
    sem_close(sem_s2s3_empty); sem_close(sem_s2s3_full);
    sem_close(sem_s3p_empty);  sem_close(sem_s3p_full);

    sem_unlink(SEM_S1S2_EMPTY); sem_unlink(SEM_S1S2_FULL);
    sem_unlink(SEM_S2S3_EMPTY); sem_unlink(SEM_S2S3_FULL);
    sem_unlink(SEM_S3P_EMPTY);  sem_unlink(SEM_S3P_FULL);

    shm_unlink(SHM_S1_S2_NAME);
    shm_unlink(SHM_S2_S3_NAME);
    shm_unlink(SHM_S3_P_NAME);

    return 0;
}
