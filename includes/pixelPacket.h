#include <iostream>
#include <cstdint>
#include <list>

#ifndef pixelPacket_H

#define pixelPacket_H

class pixelPacket{
    int col;
    uint8_t r, g, b;

    std::size_t checksum;

    bool is_last;

    public:
        pixelPacket(int col,uint8_t r,uint8_t g,uint8_t b);

        pixelPacket(bool is_last);

        int get_col();

        uint8_t get_r();
        uint8_t get_g();
        uint8_t get_b();

        std::size_t get_checksum();
    private:
        std::size_t calculate_checksum();
}; 


class rowPacket{

    int row;
    int max_cols;

    std::list<pixelPacket> pixels;

    std::size_t hash;

    bool is_last;

    public:
        rowPacket(int row,int max_cols);

        rowPacket(bool is_last);

        
        void add_pixel_packet(pixelPacket pkt);

        // getters
        int get_row_no();
        int get_max_cols();

        pixelPacket get_pixel_packet();
        bool is_last_row_packet();

        void set_hash();
        bool verify_hash();

    private:
        std::size_t calculate_hash();

};

#endif