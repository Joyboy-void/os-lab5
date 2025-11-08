#include "rowPacket.h"
#include <algorithm>
#include <sstream>

rowPacket::rowPacket(int start_row_, int num_rows_, int cols_per_row_): 
    start_row(start_row_), num_rows(num_rows_), cols_per_row(cols_per_row_),
    pixels(static_cast<size_t>(std::max(0, num_rows_)) * std::max(0, cols_per_row_) * 3, 0), // initilize 0's
    hash(0),
    is_last(false)
{}

rowPacket::rowPacket(std::istringstream iss){
   iss >> start_row >> num_rows >> cols_per_row >> hash >> is_last;

    std::string token;
    while(iss >> token && token != "#"){
        pixels.push_back(static_cast<uint8_t>(std::stoi(token)));
    }
}

rowPacket::rowPacket(bool is_last_flag): 
    start_row(-1), num_rows(0), cols_per_row(0), pixels(), hash(0), is_last(is_last_flag)
{}

std::string rowPacket::encode(){
    std::ostringstream oss;

    // encoading start_row num_rows cols_per_row hash is_last pixels......
    oss << start_row << " " << num_rows << " " << cols_per_row << " " << hash << " "<< is_last << " ";

    for(uint8_t px : pixels)
        oss << static_cast<int>(px)<< " ";  

    oss << "#"; // sentinal symbol

    return oss.str();
}