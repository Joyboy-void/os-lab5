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

#include<cstdlib>
#include<string>
#include<unistd.h>

#include "../includes/libppm.h"   
#include "../includes/rowPacket.h"


extern const bool USE_HASH = true;         
extern const int MAX_ITERATIONS = 1;
extern const int MAX_QUEUE_SIZE = 512;
extern const int PROCESSED_ROW_COUNT = 8;   // number of rows batched per rowPacket
extern const int SCALING_FACTOR = 2;
// ---------------------------------------------------------------------------------


int fd_S1_S2[2],fd_S2_S3[2];


// using FNV hash function
std::size_t calculate_hash_for_packet(const rowPacket &rp) {
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


image_t* input_image; 
image_t* output_image;

int main(int argc, char **argv)
{
    if(argc != 3){
        std::cout << "usage: ./a.out <path-to-original-image> <path-to-transformed-image>\n\n";
        exit(0);
    }

    auto start_r = std::chrono::steady_clock::now();
    input_image = read_ppm_file(argv[1]);
    auto finish_r = std::chrono::steady_clock::now();

    int height = input_image->height , width = input_image->width;


    // allocate size for output image 
    output_image = new image_t;

    output_image->height = height;  
    output_image->width = width;

    output_image->image_pixels = new uint8_t**[height];
    for(int i = 0;i < height;i++){
        output_image->image_pixels[i] = new uint8_t*[width];
        for(int j = 0;j < width;j++)
            output_image->image_pixels[i][j] = new uint8_t[3]();
    }

    // create pipes 
    pipe(fd_S1_S2);
    pipe(fd_S2_S3);

    // for total time
    auto start_p = std::chrono::steady_clock::now();

    for(int i = 0;i < MAX_ITERATIONS;i++){
        
    }

    close(fd_S1_S2[0]);
    close(fd_S1_S2[1]);
    close(fd_S2_S3[0]);
    close(fd_S2_S3[1]);

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
