#include <iostream>
#include <cstdint>

#include "pixelPacket.h"


// pixelPacket Class

pixelPacket::pixelPacket(int col,uint8_t r,uint8_t g,uint8_t b) : col(col), r(r), g(g), b(b) {
    this->is_last = false;
    this->checksum = calculate_checksum();
}
pixelPacket::pixelPacket(bool is_last) : col(-1), r(0), g(0), b(0), is_last(is_last) {}

std::size_t pixelPacket::calculate_checksum(){
    return ((std::size_t)col * 19349663) ^
           ((std::size_t)r   * 83492791) ^
           ((std::size_t)g   * 2654435761) ^
           ((std::size_t)b   * 97531);
}

std::size_t pixelPacket::get_checksum(){
    return checksum;
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


// rowPacket class
rowPacket::rowPacket(int row,int max_cols) : row(row), max_cols(max_cols), is_last(false){}

rowPacket::rowPacket(bool is_last) : row(-1), max_cols(-1), is_last(is_last){}

void rowPacket::add_pixel_packet(pixelPacket pkt){
    if(! (this->pixels.size() < max_cols)){
        std::cerr << "rowPacket is full cant add more pixelPackets" << std::endl;
        return ;
    }

    this->pixels.push_back(pkt);    
}

pixelPacket rowPacket::get_pixel_packet(){
    if(pixels.size() == 0)
        return pixelPacket(true);
    
    pixelPacket temp = pixels.front();
    pixels.pop_front();

    return temp;
}
int rowPacket::get_row_no(){
    return this->row;
}
int rowPacket::get_max_cols(){
    return this->max_cols;
}


bool rowPacket::is_last_row_packet(){
    return is_last;
}


void rowPacket::set_hash(){
    this->hash = this->calculate_hash();
}
bool rowPacket::verify_hash(){
    return this->calculate_hash() == this->hash;
}
std::size_t rowPacket::calculate_hash(){
    std::size_t hsh = 0;
    for(pixelPacket e : pixels)
        hsh ^= e.get_checksum();

    return hsh;
}
