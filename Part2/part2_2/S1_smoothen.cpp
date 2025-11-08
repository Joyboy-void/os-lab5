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

extern int fd_S1_S2[2];

std::size_t calculate_hash_for_packet(const rowPacket &rp);


extern image_t* input_image;

void main(){
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

        // encode the rowPacket
        std::string enc_pkt = rpkt.encode();

        uint32_t length = enc_pkt.size();

        //write size of encoded rowPacket
        write(fd_S1_S2[1], &length, sizeof(length));

        //write encoding of rowPacket
        write(fd_S1_S2[1],enc_pkt.c_str(),length);

        i += take;
    }

    // push terminal packet
    rowPacket ter(true);
    std::string enc_ter = ter.encode();

    uint32_t len = enc_ter.size();

    write(fd_S1_S2[1], &len, sizeof(len));
    write(fd_S1_S2[1], enc_ter.c_str(), len);
}