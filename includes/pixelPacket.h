#include <iostream>
#include <cstdint>

#ifndef pixelPacket_H

#define pixelPacket_H

class pixelPacket{

    int row;
    int col;

    uint8_t r, b, g;

    std::size_t hash;

    bool is_last;

    public:
        pixelPacket(int row, int col, uint8_t r, uint8_t b, uint8_t g);

        pixelPacket(bool is_last);

        // getters
        int get_row();
        int get_col();
        uint8_t get_r();
        uint8_t get_b();
        uint8_t get_g();
        bool is_last_packet();

        bool verify_hash();

    private:
        std::size_t calculate_hash();

};

#endif