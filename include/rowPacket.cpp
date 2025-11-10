#include "rowPacket.h"
#include <algorithm>
#include <sstream>

rowPacket::rowPacket(int start_row_, int num_rows_, int cols_per_row_): 
    start_row(start_row_), num_rows(num_rows_), cols_per_row(cols_per_row_),
    pixels(static_cast<size_t>(std::max(0, num_rows_)) * std::max(0, cols_per_row_) * 3, 0), // initilize 0's
    hash(0),
    is_last(false)
{}

rowPacket::rowPacket(bool is_last_flag): 
    start_row(-1), num_rows(0), cols_per_row(0), pixels(), hash(0), is_last(is_last_flag)
{}