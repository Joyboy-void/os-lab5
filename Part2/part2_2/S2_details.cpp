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

extern int fd_S1_S2[2],fd_S2_S3[2];

std::size_t calculate_hash_for_packet(const rowPacket &rp);


extern image_t* input_image;

void main(){
    int width = input_image->width;
    int height = input_image->height;

    const int cols_per_row = std::max(0, width - 2);

    while (true) {

        uint32_t length;

        auto n = read(fd_S1_S2[0], &length, sizeof(length));
        if(n != sizeof(length)){
            std::cerr << "Read error.1" << std::endl;
            exit(1);
        }

        std::vector<char> buffer(length);

        n = read(fd_S1_S2[0], buffer.data(), length);
        if(n != length){
            std::cerr << "Read error.2" << std::endl;
            exit(1);
        }
        std::string buffer_str(buffer.begin(),buffer.end());
        std::istringstream iss(buffer_str);

        rowPacket rpkt(std::move(iss));

        if (rpkt.is_last) {
            // forward terminal
            rowPacket ter(true);
            std::string enc_ter = ter.encode();

            uint32_t len = enc_ter.size();

            write(fd_S2_S3[1], &len, sizeof(len));
            write(fd_S2_S3[1], enc_ter.c_str(), len);

            return;
        }

        // verify hash (if USE_HASH)
        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";

                // forward treminate and exit
                rowPacket ter(true);
                std::string enc_ter = ter.encode();

                uint32_t len = enc_ter.size();

                write(fd_S2_S3[1], &len, sizeof(len));
                write(fd_S2_S3[1], enc_ter.c_str(), len);

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
        

        std::string enc_out_rpkt = out_rpkt.encode();

        uint32_t len = enc_out_rpkt.size();

        write(fd_S2_S3[1], &len, sizeof(len));
        write(fd_S2_S3[1], enc_out_rpkt.c_str(), len);
    }
}