#include <iostream>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <cerrno>

#include "../../include/rowPacket.h"
#include "../../include/libppm.h"   

int fd_S1_S2[2], fd_S2_S3[2], fd_S3_P[2];

const bool USE_HASH = true;         
const int MAX_ITERATIONS = 1;
const int PROCESSED_ROW_COUNT = 32; 
const int SCALING_FACTOR = 2;

// FNV hash function
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

// read/write helpers to counter partial reads/writes
static ssize_t write_all(int fd, const void *buf, size_t count) {

    const char *p = static_cast<const char*>(buf);
    size_t written = 0;
    while (written < count) {
        ssize_t w = write(fd, p + written, count - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += static_cast<size_t>(w);
    }
    return static_cast<ssize_t>(written);
}

static ssize_t read_all(int fd, void *buf, size_t count) {
    char *p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < count) {
        ssize_t r = read(fd, p + got, count - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return static_cast<ssize_t>(got); // EOF
        got += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(got);
}

//**  header formate: int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last

static const size_t HDR_SIZE = sizeof(int32_t)*3 + sizeof(uint64_t) + sizeof(uint8_t);

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
        char hdr[HDR_SIZE];
        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        write_all(fd_S1_S2[1], hdr, HDR_SIZE);
        return;
    }

    const int cols_per_row = std::max(0, width - 2);
    const size_t fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * cols_per_row * 3;

    int dir[9][2] = {
        {-1,-1}, {-1,0}, {-1,1},
        {0,-1}, {0,0}, {0,1},
        {1,-1}, {1,0}, {1,1}
    };

    for (int i = 1; i <= height - 2; ) {
        int batch_start = i;
        int take = std::min(PROCESSED_ROW_COUNT, (height - 1) - i + 0);
        if (take <= 0) break;

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

        std::vector<char> outbuf(HDR_SIZE + fixed_payload);
        serialize_header(outbuf.data(), rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row, (uint64_t)rpkt.hash, rpkt.is_last ? 1 : 0);

        size_t actual_bytes = static_cast<size_t>(take) * cols_per_row * 3;
        if (actual_bytes > 0) {
            memcpy(outbuf.data() + HDR_SIZE, rpkt.pixels.data(), actual_bytes);
        }
        if (fixed_payload > actual_bytes) {
            memset(outbuf.data() + HDR_SIZE + actual_bytes, 0, fixed_payload - actual_bytes);
        }

        if (write_all(fd_S1_S2[1], outbuf.data(), outbuf.size()) < 0) {
            perror("S1 write_all");
            _exit(1);
        }

        i += take;
    }


    {
        int32_t start_row = -1, num_rows = 0, cols = 0;
        uint64_t hash = 0;
        uint8_t is_last = 1;
        std::vector<char> termbuf(HDR_SIZE + (size_t)PROCESSED_ROW_COUNT * std::max(0, (int)( (input_image->width>2) ? input_image->width-2 : 0)) * 3 );
        serialize_header(termbuf.data(), start_row, num_rows, cols, hash, is_last);
        
        write_all(fd_S1_S2[1], termbuf.data(), termbuf.size());
    }
}

void S2_find_details(image_t* input_image) {

    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        // forward terminal
        char hdr[HDR_SIZE];
        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        write_all(fd_S2_S3[1], hdr, HDR_SIZE);
        return;
    }

    const int cols_per_row = std::max(0, width - 2);
    const size_t fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * cols_per_row * 3;

    std::vector<char> hdrbuf(HDR_SIZE);
    std::vector<uint8_t> payloadbuf(fixed_payload);

    while (true) {
        
        ssize_t got = read_all(fd_S1_S2[0], hdrbuf.data(), HDR_SIZE);
        if (got <= 0) { 
            // forward terminal and exit
            char thdr[HDR_SIZE];
            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            write_all(fd_S2_S3[1], thdr, HDR_SIZE);
            return;
        }
        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        deserialize_header(hdrbuf.data(), start_row, num_rows, cols, hash, is_last);

        // read payload
        if (read_all(fd_S1_S2[0], payloadbuf.data(), fixed_payload) != (ssize_t)fixed_payload) {
            perror("S2 payload read");
            _exit(1);
        }

        if (is_last) {
            // forward terminal header + zero payload
            char thdr[HDR_SIZE];
            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            std::vector<char> termbuf(HDR_SIZE + fixed_payload);
            memcpy(termbuf.data(), thdr, HDR_SIZE);
           
            write_all(fd_S2_S3[1], termbuf.data(), termbuf.size());
            return;
        }

        // construct rpkt from payload 
        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual_bytes = static_cast<size_t>(num_rows) * cols * 3;
        if (actual_bytes > 0) {
            memcpy(rpkt.pixels.data(), payloadbuf.data(), actual_bytes);
        }
        rpkt.hash = (std::size_t)hash;

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S2: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";
                
                char thdr[HDR_SIZE];
                serialize_header(thdr, -1, 0, 0, 0ULL, 1);
                std::vector<char> termbuf(HDR_SIZE + fixed_payload);
                memcpy(termbuf.data(), thdr, HDR_SIZE);
                write_all(fd_S2_S3[1], termbuf.data(), termbuf.size());
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
                
                if (dR < 0) dR = 0;
                if (dG < 0) dG = 0; 
                if (dB < 0) dB = 0;

                uint8_t* op = out_rpkt.pixel_ptr(r_off, cidx);

                op[0] = (uint8_t)dR; 
                op[1] = (uint8_t)dG; 
                op[2] = (uint8_t)dB;
            }
        }

        if (USE_HASH) 
            out_rpkt.hash = calculate_hash_for_packet(out_rpkt);

        std::vector<char> outbuf(HDR_SIZE + fixed_payload);

        serialize_header(outbuf.data(), out_rpkt.start_row, out_rpkt.num_rows, out_rpkt.cols_per_row, (uint64_t)out_rpkt.hash, out_rpkt.is_last ? 1 : 0);
        size_t out_actual = static_cast<size_t>(out_rpkt.num_rows) * out_rpkt.cols_per_row * 3;

        if (out_actual > 0) 
            memcpy(outbuf.data() + HDR_SIZE, out_rpkt.pixels.data(), out_actual);
        if (fixed_payload > out_actual) 
            memset(outbuf.data() + HDR_SIZE + out_actual, 0, fixed_payload - out_actual);

        if (write_all(fd_S2_S3[1], outbuf.data(), outbuf.size()) < 0) {
            perror("S2 write_all");
            _exit(1);
        }
    }
}

void S3_sharpen(image_t* input_image) {
    int width = input_image->width;
    int height = input_image->height;
    if (height < 3 || width < 3) {
        // forward terminal
        char thdr[HDR_SIZE];
        serialize_header(thdr, -1, 0, 0, 0ULL, 1);
        write_all(fd_S3_P[1], thdr, HDR_SIZE);
        return;
    }

    const int cols_per_row = std::max(0, width - 2);
    const size_t fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * cols_per_row * 3;

    std::vector<char> hdrbuf(HDR_SIZE);
    std::vector<uint8_t> payloadbuf(fixed_payload);

    while (true) {
        if (read_all(fd_S2_S3[0], hdrbuf.data(), HDR_SIZE) != (ssize_t)HDR_SIZE) {
            // forward terminal and exit
            char thdr[HDR_SIZE];
            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            write_all(fd_S3_P[1], thdr, HDR_SIZE);
            return;
        }
        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        deserialize_header(hdrbuf.data(), start_row, num_rows, cols, hash, is_last);

        if (read_all(fd_S2_S3[0], payloadbuf.data(), fixed_payload) != (ssize_t)fixed_payload) {
            perror("S3 payload read");
            _exit(1);
        }

        if (is_last) {
            char thdr[HDR_SIZE];

            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            std::vector<char> termbuf(HDR_SIZE + fixed_payload);
            memcpy(termbuf.data(), thdr, HDR_SIZE);
            write_all(fd_S3_P[1], termbuf.data(), termbuf.size());
            return;
        }

        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual = static_cast<size_t>(num_rows) * cols * 3;

        if (actual > 0) memcpy(rpkt.pixels.data(), payloadbuf.data(), actual);
        rpkt.hash = (std::size_t)hash;

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S3: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";
                char thdr[HDR_SIZE];
                serialize_header(thdr, -1, 0, 0, 0ULL, 1);
                std::vector<char> termbuf(HDR_SIZE + fixed_payload);
                memcpy(termbuf.data(), thdr, HDR_SIZE);
                write_all(fd_S3_P[1], termbuf.data(), termbuf.size());
                return;
            }
        }

        // compute sharpened packet
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

        // write header+payload once
        std::vector<char> outbuf(HDR_SIZE + fixed_payload);
        serialize_header(outbuf.data(), out_rpkt.start_row, out_rpkt.num_rows, out_rpkt.cols_per_row, (uint64_t)out_rpkt.hash, out_rpkt.is_last ? 1 : 0);
        size_t out_actual = static_cast<size_t>(out_rpkt.num_rows) * out_rpkt.cols_per_row * 3;

        if (out_actual > 0) 
            memcpy(outbuf.data() + HDR_SIZE, out_rpkt.pixels.data(), out_actual);
        if (fixed_payload > out_actual) 
            memset(outbuf.data() + HDR_SIZE + out_actual, 0, fixed_payload - out_actual);

        if (write_all(fd_S3_P[1], outbuf.data(), outbuf.size()) < 0) {
            perror("S3 write_all");
            _exit(1);
        }
    }
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

    // prepare output image (copy input to preserve edges)
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

    // create pipes
    if (pipe(fd_S1_S2) < 0) { perror("pipe1"); exit(1); }
    if (pipe(fd_S2_S3) < 0) { perror("pipe2"); exit(1); }
    if (pipe(fd_S3_P)  < 0) { perror("pipe3"); exit(1); }

    auto start_p = std::chrono::steady_clock::now();

    // fork S1
    pid_t pid1 = fork();
    if (pid1 < 0) { perror("fork1"); exit(1); }
    if (pid1 == 0) {
        close(fd_S1_S2[0]);
        close(fd_S2_S3[0]); close(fd_S2_S3[1]);
        close(fd_S3_P[0]); close(fd_S3_P[1]);
        S1_smoothen(input_image);
        _exit(0);
    }

    // fork S2
    pid_t pid2 = fork();
    if (pid2 < 0) { perror("fork2"); exit(1); }
    if (pid2 == 0) {
        close(fd_S1_S2[1]);
        close(fd_S2_S3[0]);
        close(fd_S3_P[0]); close(fd_S3_P[1]);
        S2_find_details(input_image);
        _exit(0);
    }

    // fork S3
    pid_t pid3 = fork();
    if (pid3 < 0) { perror("fork3"); exit(1); }
    if (pid3 == 0) {
        close(fd_S1_S2[0]); close(fd_S1_S2[1]);
        close(fd_S2_S3[1]);
        close(fd_S3_P[0]);
        S3_sharpen(input_image);
        _exit(0);
    }

    // parent: close unused
    close(fd_S1_S2[0]); close(fd_S1_S2[1]);
    close(fd_S2_S3[0]); close(fd_S2_S3[1]);
    close(fd_S3_P[1]);

    
    const int cols_per_row = std::max(0, width - 2);
    const size_t fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * cols_per_row * 3;
    
    std::vector<char> hdrbuf(HDR_SIZE);
    std::vector<uint8_t> payloadbuf(fixed_payload);

    while (true) {
        if (read_all(fd_S3_P[0], hdrbuf.data(), HDR_SIZE) != (ssize_t)HDR_SIZE) 
            break;
        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;
        deserialize_header(hdrbuf.data(), start_row, num_rows, cols, hash, is_last);

        if (read_all(fd_S3_P[0], payloadbuf.data(), fixed_payload) != (ssize_t)fixed_payload) {
            perror("parent payload read");
            break;
        }

        if (is_last) break;

        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual = static_cast<size_t>(num_rows) * cols * 3;
        if (actual > 0) memcpy(rpkt.pixels.data(), payloadbuf.data(), actual);

        if (USE_HASH) {
            if (calculate_hash_for_packet(rpkt) != (std::size_t)hash) {
                std::cerr << "Parent: data corrupted for row " << rpkt.start_row << "\n";
                break;
            }
        }

        // copy into output_image
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

    close(fd_S3_P[0]);

    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);
    waitpid(pid3, nullptr, 0);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;

    std::cout << "Total Processing time per iteration " << elapsed.count()*1000/MAX_ITERATIONS << " ms\n";

    write_ppm_file(argv[2], output_image);
    std::cout << "Image written to " << argv[2] << std::endl;

    return 0;
}