#include <iostream>
#include <list>
#include <cstdint>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include<chrono>

#include "../includes/libppm.h"
#include "../includes/pixelPacket.h"


const int MAX_ITERATIONS = 1;
const int MAX_QUEUE_SIZE = 256;
const int PROCESSED_ROW_COUNT = 2;
const int SCALING_FACTOR = 1;

std::queue<rowPacket> q_s1_s2,q_s2_s3;

std::mutex mtx_s1_s2,mtx_s2_s3; 

std::condition_variable cv_empty_s1_s2,cv_fill_s1_s2;
std::condition_variable cv_empty_s2_s3,cv_fill_s2_s3;

void S1_smoothen(image_t *input_image){

    int width = input_image->width;
    int height = input_image->height;
    
    int dir[9][2] = {
            {-1,-1}, {-1, 0}, {-1, 1},
            {0 ,-1}, {0 , 0}, {0 , 1},
            {1 ,-1}, {1 , 0}, {1 , 1}
    };


    for(int i=1;i<height-1;i++){

        rowPacket rpkt(i,width-1);
        
        for(int j=1;j<width-1;j++){
            int r=0,b=0,g=0;
            for(int k = 0;k < 9;k++){
                r += input_image->image_pixels[i + dir[k][0]][j + dir[k][1]][0];
                b += input_image->image_pixels[i + dir[k][0]][j + dir[k][1]][1];
                g += input_image->image_pixels[i + dir[k][0]][j + dir[k][1]][2];
            }
            pixelPacket pkt(j,(uint8_t)(r/9),(uint8_t)(g/9),(uint8_t)(b/9));

            rpkt.add_pixel_packet(pkt);
        }

        // compute hash before adding to queue
        rpkt.set_hash();

        std::unique_lock<std::mutex> lock(mtx_s1_s2);
        cv_empty_s1_s2.wait(lock, []{ return q_s1_s2.size() < MAX_QUEUE_SIZE; });

        q_s1_s2.push(rpkt);

        lock.unlock();
        cv_fill_s1_s2.notify_one();
    }

    {
        std::lock_guard<std::mutex> lock(mtx_s1_s2);
        q_s1_s2.push(rowPacket{true});
    }
    cv_fill_s1_s2.notify_one();
}


void S2_find_details( image_t *input_image){
    while(true){
        std::unique_lock<std::mutex> lock(mtx_s1_s2);
        cv_fill_s1_s2.wait(lock,[]{return !q_s1_s2.empty();});

        rowPacket rpkt = q_s1_s2.front();
        q_s1_s2.pop();
        lock.unlock();
        cv_empty_s1_s2.notify_one();

        if(rpkt.is_last_row_packet()){
            std::lock_guard<std::mutex> lock1(mtx_s2_s3);

            q_s2_s3.push(rowPacket(true));
            cv_fill_s2_s3.notify_one();

            break;
        }

        if(!rpkt.verify_hash()){
            std::cerr << "Data Corrupted in rowPacket("<<rpkt.get_row_no()<<")!!" << std::endl;
            break;
        }

        int i = rpkt.get_row_no();

        rowPacket new_rpkt(i,rpkt.get_max_cols());

        for(int j = 1;j < rpkt.get_max_cols();j++){

            pixelPacket pkt = rpkt.get_pixel_packet();

            int diff_r = ((input_image->image_pixels[i][j][0] - pkt.get_r()) < 0) ? 0 : (input_image->image_pixels[i][j][0] - pkt.get_r());
            int diff_g = ((input_image->image_pixels[i][j][1] - pkt.get_g()) < 0) ? 0 : (input_image->image_pixels[i][j][1] - pkt.get_g());
            int diff_b = ((input_image->image_pixels[i][j][2] - pkt.get_b()) < 0) ? 0 : (input_image->image_pixels[i][j][2] - pkt.get_b());

            pixelPacket new_pkt(j,(uint8_t)diff_r,(uint8_t)diff_g,(uint8_t)diff_b);

            new_rpkt.add_pixel_packet(new_pkt);
        }

        // compute and update hash.
        new_rpkt.set_hash(); 

        std::unique_lock<std::mutex> lock1(mtx_s2_s3);

        cv_empty_s2_s3.wait(lock1, [] { return q_s2_s3.size() < MAX_QUEUE_SIZE; });
        q_s2_s3.push(new_rpkt);
        
        lock1.unlock();
        cv_fill_s2_s3.notify_one();
    }
}


void S3_sharpen (image_t *input_image, image_t *output_image) {

    while(true){
        std::unique_lock<std::mutex> lock1(mtx_s2_s3);
        cv_fill_s2_s3.wait(lock1 , []{ return !q_s2_s3.empty(); });

        rowPacket rpkt = q_s2_s3.front();
        q_s2_s3.pop();

        lock1.unlock();
        cv_empty_s2_s3.notify_one();

        if(rpkt.is_last_row_packet())
            break;

        if(!rpkt.verify_hash()){
            std::cerr << "Data Corrupted in pixelPacket("<<rpkt.get_row_no() << ")!!" << std::endl;
            break;
        }

        int i = rpkt.get_row_no();

        for(int j = 1;j < rpkt.get_max_cols(); j++){

            pixelPacket pkt = rpkt.get_pixel_packet();

            output_image->image_pixels[i][j][0]= ((input_image->image_pixels[i][j][0] + (SCALING_FACTOR * pkt.get_r())) > 255) ? 255 : (input_image->image_pixels[i][j][0] + (SCALING_FACTOR * pkt.get_r()));
            output_image->image_pixels[i][j][1]= ((input_image->image_pixels[i][j][1] + (SCALING_FACTOR * pkt.get_g())) > 255) ? 255 : (input_image->image_pixels[i][j][1] + (SCALING_FACTOR * pkt.get_g()));
            output_image->image_pixels[i][j][2]= ((input_image->image_pixels[i][j][2] + (SCALING_FACTOR * pkt.get_b())) > 255) ? 255 : (input_image->image_pixels[i][j][2] + (SCALING_FACTOR * pkt.get_b()));
        }
    }
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
    
    int height = input_image->height , width = input_image->width;

    image_t* output_image = new image_t;

    output_image->height = height;  
    output_image->width = width;

    output_image->image_pixels = new uint8_t**[height];
    for(int i = 0;i < height;i++){
        output_image->image_pixels[i] = new uint8_t*[width];
        for(int j = 0;j < width;j++)
            output_image->image_pixels[i][j] = new uint8_t[3]();
    }

    // for total time
    auto start_p = std::chrono::steady_clock::now();

    for(int i = 0;i < MAX_ITERATIONS;i++){

        std:: thread t1(S1_smoothen,input_image);
        std:: thread t2(S2_find_details,input_image);
        std:: thread t3(S3_sharpen,input_image,output_image);

        t1.join();
        t2.join();
        t3.join();
    }

    auto finish_p = std::chrono::steady_clock::now();

    // for write time
    auto start_w = std::chrono::steady_clock::now();

    write_ppm_file(argv[2],output_image);

    auto finish_w = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_seconds_write = finish_w - start_w;

    std::chrono::duration<double> elapsed = finish_p - start_p;
    std::cout << "Total Processing time per iteration " << elapsed.count()*1000/MAX_ITERATIONS << " ms\n";

    return 0;
}