#include <cstdint>
#include <cstddef>
#include <string>
#include <sstream>
#include <vector>

#ifndef ROWPACKET_H
#define ROWPACKET_H

class rowPacket {
public:
    int start_row;        
    int num_rows;         
    int cols_per_row;     
    std::vector<uint8_t> pixels; 
    std::size_t hash;     
    bool is_last;         // sentinel packet


    rowPacket(int start_row_, int num_rows_, int cols_per_row_);

    explicit rowPacket(bool is_last_flag);

    // helper to get pointer to RGB triplet for given row_offset (0..num_rows-1) and col_index (0..cols_per_row-1)
    inline uint8_t* pixel_ptr(int row_offset, int col_index) {
        size_t idx = (static_cast<size_t>(row_offset) * cols_per_row + static_cast<size_t>(col_index)) * 3;
        return &pixels[idx];
    }

};

#endif 