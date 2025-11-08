#include <iostream>
#include <cstdint>
#include <list>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cassert>

#include "../includes/libppm.h"   
#include "../includes/rowPacket.h"


const bool USE_HASH = true;         
const int MAX_ITERATIONS = 1;
const int MAX_QUEUE_SIZE = 512;
const int PROCESSED_ROW_COUNT = 8;   // number of rows batched per rowPacket
const int SCALING_FACTOR = 2;
// ---------------------------------------------------------------------------------


std::queue<rowPacket> q_s1_s2, q_s2_s3;
std::mutex mtx_s1_s2, mtx_s2_s3;
std::condition_variable cv_empty_s1_s2, cv_fill_s1_s2;
std::condition_variable cv_empty_s2_s3, cv_fill_s2_s3;

// using FNV hash function
static std::size_t calculate_hash_for_packet(const rowPacket &rp) {
    if (rp.pixels.empty()) return 0;

    const std::size_t FNV_offset = 1469598103934665603ULL;
    const std::size_t FNV_prime  = 1099511628211ULL;
    std::size_t h = FNV_offset;

    for (uint8_t b : rp.pixels) {
        h ^= static_cast<std::size_t>(b);
        h *= FNV_prime;
    }

    // incorporate position ,so that identical pixels in different rows are distinct
    h ^= static_cast<std::size_t>(rp.start_row + 0x9e3779b9);
    h *= FNV_prime;
    h ^= static_cast<std::size_t>(rp.num_rows + 0x9e3779b9);
    h *= FNV_prime;

    return h;
}


void S1_smoothen(image_t *input_image){
    int width = input_image->width;
    int height = input_image->height;

    // number of columns 
    const int cols_per_row = std::max(0, width - 2);

    int dir[9][2] = {
        {-1,-1}, {-1, 0}, {-1, 1},
        {0 ,-1}, {0 , 0}, {0 , 1},
        {1 ,-1}, {1 , 0}, {1 , 1}
    };

    // process rows 1 .. height-2 (interior rows)
    for (int i = 1; i <= height - 2; ) {
        int batch_start = i;
        int take = std::min(PROCESSED_ROW_COUNT, (height - 1) - i + 0); // ensure we don't go beyond height-2
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

        // compute and set hash (if enabled)
        if (USE_HASH) 
            rpkt.hash = calculate_hash_for_packet(rpkt);

        // push to queue q_s1_s2 (wait if full)
        {
            std::unique_lock<std::mutex> lock(mtx_s1_s2);
            cv_empty_s1_s2.wait(lock, []{ return q_s1_s2.size() < MAX_QUEUE_SIZE; });
            q_s1_s2.push(std::move(rpkt));
        }
        cv_fill_s1_s2.notify_one();

        i += take;
    }

    // push terminal packet
    {
        std::lock_guard<std::mutex> lock(mtx_s1_s2);
        q_s1_s2.push(rowPacket{true});
    }
    cv_fill_s1_s2.notify_one();
}


void S2_find_details(image_t *input_image){
    int width = input_image->width;
    int height = input_image->height;

    const int cols_per_row = std::max(0, width - 2);

    while (true) {
        rowPacket rpkt(false);

        {
            std::unique_lock<std::mutex> lock(mtx_s1_s2);
            cv_fill_s1_s2.wait(lock, []{ return !q_s1_s2.empty(); });
            rpkt = std::move(q_s1_s2.front());
            q_s1_s2.pop();
        }
        cv_empty_s1_s2.notify_one();

        if (rpkt.is_last) {
            // forward terminal
            {
                std::lock_guard<std::mutex> lock(mtx_s2_s3);
                q_s2_s3.push(rowPacket{true});
            }
            cv_fill_s2_s3.notify_one();
            return;
        }

        // verify hash (if USE_HASH)
        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";

                // forward treminate and exit
                {
                    std::lock_guard<std::mutex> lock(mtx_s2_s3);
                    q_s2_s3.push(rowPacket{true});
                }
                cv_fill_s2_s3.notify_one();
                return;
            }
        }

        // produce difference packet
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

        // compute hash (if USE_HASH)
        if (USE_HASH) 
            out_rpkt.hash = calculate_hash_for_packet(out_rpkt);

        // push to q_s2_s3
        {
            std::unique_lock<std::mutex> lock(mtx_s2_s3);
            cv_empty_s2_s3.wait(lock, []{ return q_s2_s3.size() < MAX_QUEUE_SIZE; });
            q_s2_s3.push(std::move(out_rpkt));
        }
        cv_fill_s2_s3.notify_one();
    }
}

void S3_sharpen (image_t *input_image, image_t *output_image) {
    int width = input_image->width;
    int height = input_image->height;

    while (true) {
        rowPacket rpkt(false);

        {
            std::unique_lock<std::mutex> lock(mtx_s2_s3);
            cv_fill_s2_s3.wait(lock, []{ return !q_s2_s3.empty(); });
            rpkt = std::move(q_s2_s3.front());
            q_s2_s3.pop();
        }

        cv_empty_s2_s3.notify_one();

        if (rpkt.is_last) 
            return;

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";
                return;
            }
        }

        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int i = rpkt.start_row + r_off;
            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {

                uint8_t* diff_p = rpkt.pixel_ptr(r_off, cidx);
                
                int j = 1 + cidx;
                
                int newR = input_image->image_pixels[i][j][0] + (SCALING_FACTOR * static_cast<int>(diff_p[0]));
                int newG = input_image->image_pixels[i][j][1] + (SCALING_FACTOR * static_cast<int>(diff_p[1]));
                int newB = input_image->image_pixels[i][j][2] + (SCALING_FACTOR * static_cast<int>(diff_p[2]));
                
                output_image->image_pixels[i][j][0] = static_cast<uint8_t>(newR > 255 ? 255 : newR);
                output_image->image_pixels[i][j][1] = static_cast<uint8_t>(newG > 255 ? 255 : newG);
                output_image->image_pixels[i][j][2] = static_cast<uint8_t>(newB > 255 ? 255 : newB);
            }
        }
    }
}


int main(int argc, char **argv)
{
    if(argc != 3){
        std::cout << "usage: ./a.out <path-to-original-image> <path-to-transformed-image>\n\n";
        exit(0);
    }

    auto start_r = std::chrono::steady_clock::now();
    image_t *input_image = read_ppm_file(argv[1]);
    auto finish_r = std::chrono::steady_clock::now();

    int height = input_image->height , width = input_image->width;

    image_t* output_image = new image_t;

    output_image->height = height;  
    output_image->width = width;

    output_image->image_pixels = new uint8_t**[height];
    for(int i = 0;i < height;i++){
        output_image->image_pixels[i] = new uint8_t*[width];
        for(int j = 0;j < width;j++)
            output_image->image_pixels[i][j] = new uint8_t[3]();
    }

    // for total time
    auto start_p = std::chrono::steady_clock::now();

    for(int i = 0;i < MAX_ITERATIONS;i++){
        std:: thread t1(S1_smoothen,input_image);
        std:: thread t2(S2_find_details,input_image);
        std:: thread t3(S3_sharpen,input_image,output_image);

        t1.join();
        t2.join();
        t3.join();
    }

    auto finish_p = std::chrono::steady_clock::now();

    // for write time
    auto start_w = std::chrono::steady_clock::now();

    write_ppm_file(argv[2],output_image);

    auto finish_w = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_seconds_write = finish_w - start_w;

    std::chrono::duration<double> elapsed = finish_p - start_p;
    std::cout << "Total Processing time per iteration " << elapsed.count()*1000/MAX_ITERATIONS << " ms\n";

    return 0;
}
