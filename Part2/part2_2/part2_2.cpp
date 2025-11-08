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
#include <cstdio>
#include <cstring>

#include "rowPacket.h"
#include "../includes/libppm.h"

// fd's
int fd_S1_S2[2], fd_S2_S3[2], fd_S3_P[2];

const bool USE_HASH = true;
const int MAX_ITERATIONS = 1;
const int PROCESSED_ROW_COUNT = 8;
const int SCALING_FACTOR = 2;

// FNV-hash function
static std::size_t calculate_hash_for_packet(const rowPacket &rp) {
    if (rp.pixels.empty()) return 0;
    const std::size_t FNV_offset = 1469598103934665603ULL;
    const std::size_t FNV_prime  = 1099511628211ULL;
    std::size_t h = FNV_offset;
    for (uint8_t b : rp.pixels) {
        h ^= static_cast<std::size_t>(b);
        h *= FNV_prime;
    }
    h ^= static_cast<std::size_t>(rp.start_row + 0x9e3779b9);
    h *= FNV_prime;
    h ^= static_cast<std::size_t>(rp.num_rows + 0x9e3779b9);
    h *= FNV_prime;
    return h;
}


void S1_smoothen (image_t* input_image){
    int width = input_image->width;
    int height = input_image->height;

    const int cols_per_row = std::max(0, width - 2);

    int dir[9][2] = {
        {-1,-1}, {-1, 0}, {-1, 1},
        {0 ,-1}, {0 , 0}, {0 , 1},
        {1 ,-1}, {1 , 0}, {1 , 1}
    };

    // wrap write-end with stdio
    FILE* out = fdopen(fd_S1_S2[1], "w");
    if (!out) { 
        perror("fdopen S1");
        exit(1); 
    }

    for (int i = 1; i <= height - 2; ) {
        int batch_start = i;
        int take = std::min(PROCESSED_ROW_COUNT, (height - 1) - i + 0);
        if (take <= 0) break;

        rowPacket rpkt(batch_start, take, cols_per_row);

        for (int r_off = 0; r_off < take; r_off++) {
            int r = batch_start + r_off;
            for (int col = 1, cidx = 0; col <= width - 2; col++, cidx++) {
                int sumR = 0, sumG = 0, sumB = 0;
                for (int k = 0; k < 9; ++k) {
                    int ii = r + dir[k][0];
                    int jj = col + dir[k][1];
                    sumR += input_image->image_pixels[ii][jj][0];
                    sumG += input_image->image_pixels[ii][jj][1];
                    sumB += input_image->image_pixels[ii][jj][2];
                }
                uint8_t sr = static_cast<uint8_t>(sumR / 9);
                uint8_t sg = static_cast<uint8_t>(sumG / 9);
                uint8_t sb = static_cast<uint8_t>(sumB / 9);
                uint8_t *p = rpkt.pixel_ptr(r_off, cidx);
                p[0] = sr; p[1] = sg; p[2] = sb;
            }
        }

        if (USE_HASH) rpkt.hash = calculate_hash_for_packet(rpkt);

        
        fprintf(out, "%d %d %d %llu %d ", rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row, (unsigned long long)rpkt.hash, rpkt.is_last ? 1 : 0);

        for (size_t idx = 0; idx < rpkt.pixels.size(); ++idx)
            fprintf(out, "%d ", (int)rpkt.pixels[idx]);

        fputc('#', out);
        fputc('\n', out);
        fflush(out);

        i += take;
    }

    // terminal packet
    rowPacket ter(true);
    fprintf(out, "%d %d %d %llu %d #\n", -1, 0, 0, (unsigned long long)0, 1);
    fflush(out);
}


void S2_find_details(image_t* input_image){
    int width = input_image->width;
    int height = input_image->height;
    const int cols_per_row = std::max(0, width - 2);

    FILE* in  = fdopen(fd_S1_S2[0], "r");
    FILE* out = fdopen(fd_S2_S3[1], "w");
    if (!in || !out) { 
        perror("fdopen S2"); exit(1); 
    }

    while (true) {
        int start_row, num_rows, cols, is_last_int;
        size_t hash;

        
        if (fscanf(in, "%d %d %d %zu %d", &start_row, &num_rows, &cols, &hash, &is_last_int) != 5) {
            // parse error — forward terminal and exit
            fprintf(out, "%d %d %d %llu %d #\n", -1, 0, 0, (unsigned long long)0, 1);
            fflush(out);
            return;
        }

        bool is_last = (is_last_int != 0);

        if (is_last) {
            // forward terminal
            fprintf(out, "%d %d %d %llu %d #\n", -1, 0, 0, (unsigned long long)0, 1);
            fflush(out);
            return;
        }

        // read exact number of pixel tokens
        rowPacket rpkt(start_row, num_rows, cols);
        for (size_t i = 0; i < rpkt.pixels.size(); ++i) {
            int v;
            if (fscanf(in, "%d", &v) != 1) {
                std::cerr << "S2 pixel read error\n";
                exit(1); 
            }
            rpkt.pixels[i] = static_cast<uint8_t>(v);
        }

        // consume sentinel '#'
        int ch;
        do { 
            ch = fgetc(in); 
        } while (ch != EOF && isspace(ch));

        if (ch != '#') {
            std::cerr << "S2: missing '#'\n"; 
            exit(1); 
        }

        rpkt.hash = hash;

        // optional hash verify
        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S2: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";
                fprintf(out, "%d %d %d %llu %d #\n", -1, 0, 0, (unsigned long long)0, 1);
                fflush(out);
                return;
            }
        }

        // compute difference packet
        rowPacket out_rpkt(rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row);

        for (int r_off = 0; r_off < rpkt.num_rows; r_off++) {
            int row_idx = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; cidx++) {
                uint8_t* smooth_p = rpkt.pixel_ptr(r_off, cidx);
                int orig_col = 1 + cidx;

                int diffR = input_image->image_pixels[row_idx][orig_col][0] - static_cast<int>(smooth_p[0]);
                int diffG = input_image->image_pixels[row_idx][orig_col][1] - static_cast<int>(smooth_p[1]);
                int diffB = input_image->image_pixels[row_idx][orig_col][2] - static_cast<int>(smooth_p[2]);

                if (diffR < 0) diffR = 0;
                if (diffG < 0) diffG = 0;
                if (diffB < 0) diffB = 0;

                uint8_t* out_p = out_rpkt.pixel_ptr(r_off, cidx);

                out_p[0] = static_cast<uint8_t>(diffR);
                out_p[1] = static_cast<uint8_t>(diffG);
                out_p[2] = static_cast<uint8_t>(diffB);
            }
        }

        if (USE_HASH) out_rpkt.hash = calculate_hash_for_packet(out_rpkt);

       fprintf(out, "%d %d %d %llu %d ", rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row, (unsigned long long)rpkt.hash, rpkt.is_last ? 1 : 0);

        for (size_t i = 0; i < out_rpkt.pixels.size(); ++i)
            fprintf(out, "%d ", (int)out_rpkt.pixels[i]);

        fputc('#', out);
        fputc('\n', out);
        fflush(out);
    }
}


void S3_sharpen(image_t* input_image){
    FILE* in = fdopen(fd_S2_S3[0], "r");
    FILE* out = fdopen(fd_S3_P[1], "w");
    if (!in || !out) { 
        perror("fdopen S3");
        exit(1);
    }

    while (true) {
        int start_row, num_rows, cols, is_last_int;
        size_t hash;

        if (fscanf(in, "%d %d %d %zu %d", &start_row, &num_rows, &cols, &hash, &is_last_int) != 5) {
            // parse error: forward terminate and return
            fprintf(out, "%d %d %d %zu %d #\n", -1, 0, 0, (size_t)0, 1);
            fflush(out);
            return;
        }

        bool is_last = (is_last_int != 0);
        if (is_last) {
            fprintf(out, "%d %d %d %zu %d #\n", -1, 0, 0, (size_t)0, 1);
            fflush(out);
            return;
        }

        rowPacket rpkt(start_row, num_rows, cols);
        for (size_t i = 0; i < rpkt.pixels.size(); ++i) {
            int v;
            if (fscanf(in, "%d", &v) != 1) { std::cerr << "S3 pixel read error\n"; exit(1); }
            rpkt.pixels[i] = static_cast<uint8_t>(v);
        }

        int ch;
        do { ch = fgetc(in); } while (ch != EOF && isspace(ch));
        if (ch != '#') { std::cerr << "S3: missing '#'\n"; exit(1); }

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S3: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";
                fprintf(out, "%d %d %d %zu %d #\n", -1, 0, 0, (size_t)0, 1);
                fflush(out);
                return;
            }
        }


        rowPacket out_rpkt(rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row);

        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int i = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {
                uint8_t* diff_p = rpkt.pixel_ptr(r_off, cidx);
                int j = 1 + cidx;

                int newR = input_image->image_pixels[i][j][0] + (SCALING_FACTOR * static_cast<int>(diff_p[0]));
                int newG = input_image->image_pixels[i][j][1] + (SCALING_FACTOR * static_cast<int>(diff_p[1]));
                int newB = input_image->image_pixels[i][j][2] + (SCALING_FACTOR * static_cast<int>(diff_p[2]));

                out_rpkt.pixel_ptr(r_off, cidx)[0] = static_cast<uint8_t>(newR > 255 ? 255 : newR);
                out_rpkt.pixel_ptr(r_off, cidx)[1] = static_cast<uint8_t>(newG > 255 ? 255 : newG);
                out_rpkt.pixel_ptr(r_off, cidx)[2] = static_cast<uint8_t>(newB > 255 ? 255 : newB);
            }
        }

        if (USE_HASH) out_rpkt.hash = calculate_hash_for_packet(out_rpkt);

        // write sharpened packet to parent
        fprintf(out, "%d %d %d %zu %d ", out_rpkt.start_row, out_rpkt.num_rows,
                out_rpkt.cols_per_row, (size_t)out_rpkt.hash, out_rpkt.is_last ? 1 : 0);
        for (size_t i = 0; i < out_rpkt.pixels.size(); ++i)
            fprintf(out, "%d ", (int)out_rpkt.pixels[i]);
        fputc('#', out);
        fputc('\n', out);
        fflush(out);
    }
}

int main(int argc, char **argv){

    if(argc != 3){
        std::cout << "usage: ./a.out <path-to-original-image> <path-to-transformed-image>\n\n";
        exit(0);
    }

    auto start_r = std::chrono::steady_clock::now();
    image_t* input_image = read_ppm_file(argv[1]);
    auto finish_r = std::chrono::steady_clock::now();
    if (!input_image) {
        std::cerr << "Failed to read input image\n";
        return 1;
    }

    int height = input_image->height , width = input_image->width;

    // allocate size for output image
    image_t* output_image = new image_t;
    output_image->height = height;
    output_image->width = width;

    output_image->image_pixels = new uint8_t**[height];
    for(int i = 0;i < height;i++){
        output_image->image_pixels[i] = new uint8_t*[width];
        for(int j = 0;j < width;j++) {
            output_image->image_pixels[i][j] = new uint8_t[3]();
            // copy input borders so edges are preserved
            output_image->image_pixels[i][j][0] = input_image->image_pixels[i][j][0];
            output_image->image_pixels[i][j][1] = input_image->image_pixels[i][j][1];
            output_image->image_pixels[i][j][2] = input_image->image_pixels[i][j][2];
        }
    }

    // create pipes
    if (pipe(fd_S1_S2) < 0) {
        perror("pipe1"); 
        exit(1);
    }
    if (pipe(fd_S2_S3) < 0) { 
        perror("pipe2"); 
        exit(1);
    }
    if (pipe(fd_S3_P)  < 0) { 
        perror("pipe3");
        exit(1);
    }

    auto start_p = std::chrono::steady_clock::now();

    // fork S1
    pid_t pid1 = fork();
    if (pid1 < 0) { perror("fork1"); exit(1); }
    if (pid1 == 0) {
        // child S1
        close(fd_S1_S2[0]);
        close(fd_S2_S3[0]); close(fd_S2_S3[1]);
        close(fd_S3_P[0]); close(fd_S3_P[1]);
        S1_smoothen(input_image);
        // child exits after writing
        _exit(0);
    }

    // fork S2
    pid_t pid2 = fork();
    if (pid2 < 0) { perror("fork2"); exit(1); }
    if (pid2 == 0) {
        // child S2
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
        // child S3
        close(fd_S1_S2[0]); close(fd_S1_S2[1]);
        close(fd_S2_S3[1]);
        close(fd_S3_P[0]);
        S3_sharpen(input_image);
        _exit(0);
    }

    close(fd_S1_S2[0]); close(fd_S1_S2[1]);
    close(fd_S2_S3[0]); close(fd_S2_S3[1]);
    close(fd_S3_P[1]);

    FILE* in = fdopen(fd_S3_P[0], "r");
    if (!in) { perror("fdopen parent"); exit(1); }

    while (true) {
        int start_row, num_rows, cols, is_last_int;
        size_t hash;

        if (fscanf(in, "%d %d %d %zu %d", &start_row, &num_rows, &cols, &hash, &is_last_int) != 5) {
            break;
        }

        bool is_last = (is_last_int != 0);
        if (is_last) break;

        rowPacket rpkt(start_row, num_rows, cols);
        for (size_t i = 0; i < rpkt.pixels.size(); ++i) {
            int v;
            if (fscanf(in, "%d", &v) != 1) { std::cerr << "Parent pixel read error\n"; exit(1); }
            rpkt.pixels[i] = static_cast<uint8_t>(v);
        }

        int ch;
        do { ch = fgetc(in); } while (ch != EOF && isspace(ch));
        if (ch != '#') { std::cerr << "Parent: missing '#'\n"; exit(1); }

    
        if (USE_HASH) {
            if (calculate_hash_for_packet(rpkt) != hash) {
                std::cerr << "Parent: data corrupted for row " << rpkt.start_row << "\n";
                break;
            }
        }

        // copy into output_image
        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int i = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {
                int j = 1 + cidx;
                uint8_t* src = rpkt.pixel_ptr(r_off, cidx);
                output_image->image_pixels[i][j][0] = src[0];
                output_image->image_pixels[i][j][1] = src[1];
                output_image->image_pixels[i][j][2] = src[2];
            }
        }
    }

    // close parent's read end
    fclose(in);

    // wait for children
    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);
    waitpid(pid3, nullptr, 0);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;
    std::cout << "Total Processing time per iteration " << elapsed.count()*1000/MAX_ITERATIONS << " ms\n";

    // write output
    write_ppm_file(argv[2],output_image);

    // cleanup fd's
    close(fd_S3_P[0]);

    std::cout << "Image written to " << argv[2] << std::endl;
    return 0;
}