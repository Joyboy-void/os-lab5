#include <iostream>
#include "../includes/libppm.h"
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <semaphore>
#include<chrono>


int SCALING_FACTOR = 2;

image_t* S1_smoothen(image_t *input_image){

    int width = input_image->width;
    int height = input_image->height;

    image_t* smooth_img = new image_t;
    smooth_img->image_pixels = new uint8_t**[height];

    smooth_img->height=height;
    smooth_img->width=width;
    
    for(int i = 0; i < height; i++) {
        smooth_img->image_pixels[i] = new uint8_t*[width];
        for(int j = 0; j < width; j++)
            smooth_img->image_pixels[i][j] = new uint8_t[3]();
    }
    
    int dir[9][2] = {
            {-1,-1},
            {-1, 0},
            {-1, 1},
            {0 ,-1},
            {0 , 0},
            {0 , 1},
            {1 ,-1},
            {1 , 0},
            {1 , 1}
    };

    for(int i=1;i<height-1;i++){
        for(int j=1;j<width-1;j++){
            int r=0,g=0,b=0;
            for(int k = 0;k < 9;k++){
                r += input_image->image_pixels[i + dir[k][0]][j + dir[k][1]][0];
                g += input_image->image_pixels[i + dir[k][0]][j + dir[k][1]][1];
                b += input_image->image_pixels[i + dir[k][0]][j + dir[k][1]][2];
            }
            smooth_img->image_pixels[i][j][0]=(r)/9;
            smooth_img->image_pixels[i][j][1]=(g)/9;
            smooth_img->image_pixels[i][j][2]=(b)/9;
        }
    }

    return smooth_img;
}


image_t* S2_find_details( image_t *input_image, image_t *smoothened_image){
    
    int width=input_image->width;
    int height=input_image->height;
    
    image_t* details_img = new image_t;
    details_img->image_pixels = new uint8_t**[height];

    details_img->width=width;
    details_img->height=height;
    
    for(int i = 0; i < height; i++){
        details_img->image_pixels[i] = new uint8_t*[width];
        for(int j = 0; j < width; j++)
            details_img->image_pixels[i][j] = new uint8_t[3];
    }

    for(int i=0;i<height;i++){
        for(int j=0;j<width;j++){
            details_img->image_pixels[i][j][0]=((input_image->image_pixels[i][j][0]-smoothened_image->image_pixels[i][j][0])<0) ? 0 : (input_image->image_pixels[i][j][0] - smoothened_image->image_pixels[i][j][0]);
            details_img->image_pixels[i][j][1]=((input_image->image_pixels[i][j][1]-smoothened_image->image_pixels[i][j][1])<0) ? 0 : (input_image->image_pixels[i][j][1] - smoothened_image->image_pixels[i][j][1]);
            details_img->image_pixels[i][j][2]=((input_image->image_pixels[i][j][2]-smoothened_image->image_pixels[i][j][2])<0) ? 0 : (input_image->image_pixels[i][j][2] - smoothened_image->image_pixels[i][j][2]);
        }
    }
    return details_img;
}


image_t* S3_sharpen (image_t *input_image, image_t *details_image) {

    int width=input_image->width;
    int height=input_image->height;

    image_t* sharp_img = new image_t;
    sharp_img->image_pixels = new uint8_t**[height];
    
    sharp_img->height=height;
    sharp_img->width=width;
    
    for(int i = 0; i < height; i++) {
        sharp_img->image_pixels[i] = new uint8_t*[width];
        for(int j = 0; j < width; j++)
            sharp_img->image_pixels[i][j] = new uint8_t[3];
    }

    for(int i=0;i<height;i++){
        for(int j=0;j<width;j++){
            sharp_img->image_pixels[i][j][0]= ((input_image->image_pixels[i][j][0] + (SCALING_FACTOR * details_image->image_pixels[i][j][0])) > 255) ? 255 : (input_image->image_pixels[i][j][0] + (SCALING_FACTOR * details_image->image_pixels[i][j][0]));
            sharp_img->image_pixels[i][j][1]= ((input_image->image_pixels[i][j][1] + (SCALING_FACTOR * details_image->image_pixels[i][j][1])) > 255) ? 255 : (input_image->image_pixels[i][j][1] + (SCALING_FACTOR * details_image->image_pixels[i][j][1]));
            sharp_img->image_pixels[i][j][2]= ((input_image->image_pixels[i][j][2] + (SCALING_FACTOR * details_image->image_pixels[i][j][2])) > 255) ? 255 : (input_image->image_pixels[i][j][2] + (SCALING_FACTOR * details_image->image_pixels[i][j][2]));
        }
    }

    return sharp_img;
}

int main(int argc, char **argv)
{

    if(argc != 3){
        std::cout << "usage: ./a.out <path-to-original-image> <path-to-transformed-image>\n\n";
        exit(0);
    }
    auto start_r = std::chrono::steady_clock::now();
    image_t *input_image = read_ppm_file(argv[1]);
    auto finish_r = std::chrono::steady_clock::now();
    
    std::chrono::duration<double> elapsed_ms_read = finish_r - start_r;
    
    auto start = std::chrono::steady_clock::now();
    image_t *smoothened_image = S1_smoothen(input_image);
    auto finish = std::chrono::steady_clock::now();
    
    std::chrono::duration<double> elapsed_ms_smooth = finish - start;
    
    auto start_1 = std::chrono::steady_clock::now();
    image_t *details_image = S2_find_details(input_image, smoothened_image);
    auto finish_1= std::chrono::steady_clock::now();
    
    std::chrono::duration<double> elapsed_ms_details = finish_1 - start_1;
    
    auto start_2 = std::chrono::steady_clock::now();
    image_t *sharpened_image = S3_sharpen(input_image, details_image);
    auto finish_2{std::chrono::steady_clock::now()};
    
    std::chrono::duration<double> elapsed_ms_sharpen = finish_2 - start_2;

    auto start_w = std::chrono::steady_clock::now();
    write_ppm_file(argv[2], sharpened_image);
    auto finish_w = std::chrono::steady_clock::now();
    
    std::chrono::duration<double> elapsed_ms_write = finish_w - start_w;
    
    // std::cout<<"file read : "<<elapsed_ms_read.count()*1000<<" ms\n";
    std::cout<<"smooth : "<<elapsed_ms_smooth.count()*1000<<" ms\n";
    std::cout<<"details : "<<elapsed_ms_details.count()*1000<<" ms\n";
    std::cout<<"sharp : "<<elapsed_ms_sharpen.count()*1000<<" ms\n";
    // std::cout << "File write : " << elapsed_ms_write.count() * 1000 << " ms\n";
    std::cout<< "Processing time: " << (elapsed_ms_smooth.count() + elapsed_ms_details.count() + elapsed_ms_sharpen.count()) *1000<< " ms\n";
    std::cout<< "Total time: " << (elapsed_ms_smooth.count() + elapsed_ms_details.count() + elapsed_ms_sharpen.count() + elapsed_ms_read.count()+ elapsed_ms_write.count()) *1000<< " ms\n";
    return 0;
}
