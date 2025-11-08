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

extern bool USE_HASH;
extern const int MAX_ITERATIONS;
extern const int MAX_QUEUE_SIZE;
extern const int PROCESSED_ROW_COUNT;
extern const int SCALING_FACTOR;

extern int fd_S2_S3[2];

std::size_t calculate_hash_for_packet(const rowPacket &rp);

extern image_t* input_image;
extern image_t* output_image;

void main() {
    int width = input_image->width;
    int height = input_image->height;

    while (true) {

        uint32_t length;

        auto n = read(fd_S2_S3[0], &length, sizeof(length));
        if(n != sizeof(length)){
            std::cerr << "Read error" << std::endl;
            exit(1);
        }

        std::vector<char> buffer(length);

        n = read(fd_S2_S3[0], buffer.data(), length);
        if(n != length){
            std::cerr << "Read error" << std::endl;
            exit(1);
        }
        std::string buffer_str(buffer.begin(),buffer.end());
        std::istringstream iss(buffer_str);

        rowPacket rpkt(std::move(iss));

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