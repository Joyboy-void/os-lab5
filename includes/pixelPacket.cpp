#include <iostream>
#include <cstdint>

#include "pixelPacket.h"

pixelPacket::pixelPacket(int row, int col, uint8_t r, uint8_t b, uint8_t g) : row(row), col(col), r(r), g(g), b(b){
    this->hash = calculate_hash();
}

pixelPacket::pixelPacket(bool is_last) : row(-1),col(-1),r(0),g(0),b(0){
    this->is_last = is_last;
}

std::size_t pixelPacket::calculate_hash(){
    return ((std::size_t)row * 73856093) ^
           ((std::size_t)col * 19349663) ^
           ((std::size_t)r   * 83492791) ^
           ((std::size_t)g   * 2654435761) ^
           ((std::size_t)b   * 97531);
}

bool pixelPacket::verify_hash(){
    if(!is_last)
        return this->hash == this->calculate_hash();
    return false;
}

int pixelPacket::get_row(){
    return this->row;
}
int pixelPacket::get_col(){
    return this->col;
}

uint8_t pixelPacket::get_r(){
    return this->r;
}
uint8_t pixelPacket::get_g(){
    return this->g;
}
uint8_t pixelPacket::get_b(){
    return this->b;
}

bool pixelPacket::is_last_packet(){
    return this->is_last;
}

