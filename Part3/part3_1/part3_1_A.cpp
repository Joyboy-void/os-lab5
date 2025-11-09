#include <iostream>
#include <cstdint>
#include <cstddef>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>

// sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../includes/rowPacket.h"
#include "../includes/libppm.h"

const bool USE_HASH = true;
const int PROCESSED_ROW_COUNT = 32;
const int SCALING_FACTOR = 2;

// header formate: int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last
static const size_t HDR_SIZE = sizeof(int32_t)*3 + sizeof(uint64_t) + sizeof(uint8_t);

// named shared memory & semaphores 
static const char* SHM_S1_S2_NAME = "/shm_s1_s2";

static const char* SEM_S1S2_EMPTY = "/sem_s1s2_empty";
static const char* SEM_S1S2_FULL  = "/sem_s1s2_full";

// inherited by children
static size_t g_cols_per_row = 0;
static size_t g_fixed_payload = 0;  // = PROCESSED_ROW_COUNT * cols_per_row * 3
static size_t g_shm_size = 0;       // HDR_SIZE + fixed_payload

static char* shm_s1_s2 = nullptr;

static sem_t* sem_s1s2_empty = nullptr;
static sem_t* sem_s1s2_full  = nullptr;

// global accepted socket for S2
static int g_client_fd = -1;

// helper to send buffer

static bool send_all(int fd, const void* buf, size_t len) {

    const char* p = static_cast<const char*>(buf);
    while (len) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) 
                continue;
            perror("send");
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// FNV hash generating function
static std::size_t calculate_hash_for_packet(const rowPacket &rp) {
    if (rp.pixels.empty()) 
        return 0;
    const std::size_t FNV_offset = 1469598103934665603ULL;
    const std::size_t FNV_prime  = 1099511628211ULL;
    std::size_t h = FNV_offset;

    for (uint8_t b : rp.pixels){
        h ^= static_cast<std::size_t>(b);
        h *= FNV_prime;
    }

    h ^= static_cast<std::size_t>(rp.start_row + 0x9e3779b9);
    h *= FNV_prime;
    h ^= static_cast<std::size_t>(rp.num_rows + 0x9e3779b9);
    h *= FNV_prime;

    return h;
}

static void serialize_header(char *dst, int32_t start_row, int32_t num_rows, int32_t cols_per_row, uint64_t hash, uint8_t is_last) {

    size_t off = 0;

    memcpy(dst + off, &start_row, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(dst + off, &num_rows, sizeof(int32_t));  off += sizeof(int32_t);
    memcpy(dst + off, &cols_per_row, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(dst + off, &hash, sizeof(uint64_t)); off += sizeof(uint64_t);
    memcpy(dst + off, &is_last, sizeof(uint8_t)); off += sizeof(uint8_t);

    (void)off;
}

static void deserialize_header(const char *src, int32_t &start_row, int32_t &num_rows, int32_t &cols_per_row, uint64_t &hash, uint8_t &is_last) {
    
    size_t off = 0;

    memcpy(&start_row, src + off, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(&num_rows, src + off, sizeof(int32_t));  off += sizeof(int32_t);
    memcpy(&cols_per_row, src + off, sizeof(int32_t)); off += sizeof(int32_t);
    memcpy(&hash, src + off, sizeof(uint64_t)); off += sizeof(uint64_t);
    memcpy(&is_last, src + off, sizeof(uint8_t)); off += sizeof(uint8_t);

    (void)off;
}

// write a block into a shared buffer using semaphores

static void write_shm_block(char* shm_ptr, sem_t* sem_empty, sem_t* sem_full, const std::vector<char>& buf) {
    // wait for empty slot
    if (sem_wait(sem_empty) == -1) {
        perror("sem_wait empty");
        _exit(1);
    }
    // copy entire buffer into shared memory of size shm_size
    memcpy(shm_ptr, buf.data(), g_shm_size);

    // increment full
    if (sem_post(sem_full) == -1) {
        perror("sem_post full");
        _exit(1);
    }
}

// read a block from shared buffer into local buffer

static void read_shm_block(char* shm_ptr, sem_t* sem_empty, sem_t* sem_full, std::vector<char>& outbuf) {
    if (sem_wait(sem_full) == -1) {
        perror("sem_wait full");
        _exit(1);
    }
    memcpy(outbuf.data(), shm_ptr, g_shm_size);

    // increment empty
    if (sem_post(sem_empty) == -1) {
        perror("sem_post empty");
        _exit(1);
    }
}

void S1_smoothen(image_t* input_image) {
    
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {
        // write terminal into S1_S2
        std::vector<char> termbuf(g_shm_size);
        char hdr[HDR_SIZE];

        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        memcpy(termbuf.data(), hdr, HDR_SIZE);

        write_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, termbuf);
        
        return;
    }

    const int cols_per_row = std::max(0, width - 2);

    int dir[9][2] = {
        {-1,-1}, {-1,0}, {-1,1},
        {0,-1}, {0,0}, {0,1},
        {1,-1}, {1,0}, {1,1}
    };

    for (int i = 1; i <= height - 2; ) {
        int batch_start = i;
        int take = std::min(PROCESSED_ROW_COUNT, (height - 1) - i );

        if (take <= 0) 
            break;

        rowPacket rpkt(batch_start, take, cols_per_row);

        for (int r_off = 0; r_off < take; ++r_off) {
            int r = batch_start + r_off;
            for (int c = 1, cidx = 0; c <= width - 2; c++, cidx++) {
                int sumR=0, sumG=0, sumB=0;
                for (int k=0;k<9;++k) {

                    int rr = r + dir[k][0], cc = c + dir[k][1];

                    sumR += input_image->image_pixels[rr][cc][0];
                    sumG += input_image->image_pixels[rr][cc][1];
                    sumB += input_image->image_pixels[rr][cc][2];
                }

                uint8_t sr = static_cast<uint8_t>(sumR / 9);
                uint8_t sg = static_cast<uint8_t>(sumG / 9);
                uint8_t sb = static_cast<uint8_t>(sumB / 9);
                
                uint8_t *p = rpkt.pixel_ptr(r_off, cidx);
                
                p[0]=sr; p[1]=sg; p[2]=sb;
            }
        }

        if (USE_HASH)
            rpkt.hash = calculate_hash_for_packet(rpkt);

        std::vector<char> outbuf(g_shm_size);
        serialize_header(outbuf.data(), rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row, (uint64_t)rpkt.hash, rpkt.is_last ? 1 : 0);

        size_t actual_bytes = static_cast<size_t>(take) * cols_per_row * 3;

        if (actual_bytes > 0) 
            memcpy(outbuf.data() + HDR_SIZE, rpkt.pixels.data(), actual_bytes);

        if (g_shm_size > HDR_SIZE + actual_bytes) 
            memset(outbuf.data() + HDR_SIZE + actual_bytes, 0, g_shm_size - HDR_SIZE - actual_bytes);

        write_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, outbuf);

        i += take;
    }

    // send terminal 
    std::vector<char> termbuf(g_shm_size);
    char thdr[HDR_SIZE];
    serialize_header(thdr, -1, 0, 0, 0ULL, 1);
    memcpy(termbuf.data(), thdr, HDR_SIZE);

    write_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, termbuf);
}

void S2_find_details(image_t* input_image) {
    int width = input_image->width;
    int height = input_image->height;

    if (height < 3 || width < 3) {

        // forward terminal
        std::vector<char> termbuf(g_shm_size);
        char hdr[HDR_SIZE];
        serialize_header(hdr, -1, 0, 0, 0ULL, 1);
        memcpy(termbuf.data(), hdr, HDR_SIZE);

        // send terminal to S3 over TCP
        if (g_client_fd >= 0) 
            send_all(g_client_fd, termbuf.data(), g_shm_size);

        return;
    }

    std::vector<char> hdrbuf(g_shm_size);

    while (true) {

        // read block from S1 over shared memory
        read_shm_block(shm_s1_s2, sem_s1s2_empty, sem_s1s2_full, hdrbuf);

        int32_t start_row, num_rows, cols;
        uint64_t hash;
        uint8_t is_last;

        deserialize_header(hdrbuf.data(), start_row, num_rows, cols, hash, is_last);

        if (is_last) {

            // forward terminal header + zero payload to S3
            std::vector<char> termbuf(g_shm_size, 0);
            char thdr[HDR_SIZE];

            serialize_header(thdr, -1, 0, 0, 0ULL, 1);
            memcpy(termbuf.data(), thdr, HDR_SIZE);

            if (g_client_fd >= 0) 
                send_all(g_client_fd, termbuf.data(), g_shm_size);
            return;
        }

        rowPacket rpkt(start_row, num_rows, cols);
        size_t actual_bytes = static_cast<size_t>(num_rows) * cols * 3;

        if (actual_bytes > 0)
            memcpy(rpkt.pixels.data(), hdrbuf.data() + HDR_SIZE, actual_bytes);

        rpkt.hash = (std::size_t)hash;

        if (USE_HASH) {
            std::size_t expected = calculate_hash_for_packet(rpkt);
            if (expected != rpkt.hash) {
                std::cerr << "S2: Data Corrupted in rowPacket(start_row=" << rpkt.start_row << ")!!\n";

                std::vector<char> termbuf(g_shm_size, 0);
                char thdr[HDR_SIZE];
                serialize_header(thdr, -1, 0, 0, 0ULL, 1);
                memcpy(termbuf.data(), thdr, HDR_SIZE);

                if (g_client_fd >= 0) 
                    send_all(g_client_fd, termbuf.data(), g_shm_size);
                return;
            }
        }

        // details
        rowPacket out_rpkt(rpkt.start_row, rpkt.num_rows, rpkt.cols_per_row);

        for (int r_off = 0; r_off < rpkt.num_rows; ++r_off) {
            int row_idx = rpkt.start_row + r_off;

            for (int cidx = 0; cidx < rpkt.cols_per_row; ++cidx) {
                uint8_t* sp = rpkt.pixel_ptr(r_off, cidx);
                int orig_col = 1 + cidx;
                
                int dR = input_image->image_pixels[row_idx][orig_col][0] - (int)sp[0];
                int dG = input_image->image_pixels[row_idx][orig_col][1] - (int)sp[1];
                int dB = input_image->image_pixels[row_idx][orig_col][2] - (int)sp[2];
                
                if (dR < 0) dR = 0;
                if (dG < 0) dG = 0; 
                if (dB < 0) dB = 0;

                uint8_t* op = out_rpkt.pixel_ptr(r_off, cidx);
                op[0] = (uint8_t)dR; op[1] = (uint8_t)dG; op[2] = (uint8_t)dB;
            }
        }

        if (USE_HASH) 
            out_rpkt.hash = calculate_hash_for_packet(out_rpkt);

        std::vector<char> outbuf(g_shm_size, 0);
        serialize_header(outbuf.data(), out_rpkt.start_row, out_rpkt.num_rows, out_rpkt.cols_per_row, (uint64_t)out_rpkt.hash, out_rpkt.is_last ? 1 : 0);

        size_t out_actual = static_cast<size_t>(out_rpkt.num_rows) * out_rpkt.cols_per_row * 3;

        if (out_actual > 0) 
            memcpy(outbuf.data() + HDR_SIZE, out_rpkt.pixels.data(), out_actual);

        // send to S3 over TCP
        if (g_client_fd >= 0) {
            if (!send_all(g_client_fd, outbuf.data(), g_shm_size)) {
                std::cerr << "S2: send failed\n";
                return;
            }
        }
    }
}


// helper to create shared mem and map of size bytes, returns pointer
static char* create_and_map_shm(const char* name, size_t size) {
    
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { 
        perror("shm_open"); 
        return nullptr; 
    }

    if (ftruncate(fd, (off_t)size) == -1) {
        perror("ftruncate"); 
        close(fd); 
        return nullptr; 
    }

    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (p == MAP_FAILED) { 
        perror("mmap"); 
        close(fd); 
        return nullptr; 
    }

    close(fd);

    return static_cast<char*>(p);
}

int main(int argc, char **argv) {

    // usage: ./a.out <input.ppm> <output.ppm> [port]
    // specifing port is optional
    if (argc != 3 && argc != 4) {
        std::cout << "usage: ./a.out <input.ppm> <output.ppm> [port]\n";
        return 0;
    }

    int listen_port = (argc == 4) ? std::atoi(argv[3]) : 9090;

    image_t* input_image = read_ppm_file(argv[1]);
    if (!input_image) { std::cerr << "Failed to read input\n"; return 1; }

    int height = input_image->height, width = input_image->width;


    // set global varibales
    g_cols_per_row = static_cast<size_t>(std::max(0, width - 2));
    g_fixed_payload = static_cast<size_t>(PROCESSED_ROW_COUNT) * g_cols_per_row * 3;
    g_shm_size = HDR_SIZE + g_fixed_payload;

    // create shared memory regions using helper
    shm_s1_s2 = create_and_map_shm(SHM_S1_S2_NAME, g_shm_size);

    if (!shm_s1_s2) { 
        std::cerr << "Failed to create shared memory\n";
        return 1; 
    }

    // unlinking semaphores if any attached
    sem_unlink(SEM_S1S2_EMPTY); 
    sem_unlink(SEM_S1S2_FULL);

    // create semaphores 
    // initially empty=1, full=0
    sem_s1s2_empty = sem_open(SEM_S1S2_EMPTY, O_CREAT, 0666, 1);
    sem_s1s2_full  = sem_open(SEM_S1S2_FULL,  O_CREAT, 0666, 0);

    if (sem_s1s2_empty == SEM_FAILED || sem_s1s2_full == SEM_FAILED ) {
        perror("sem_open"); return 1;
    }

    // TCP server setup (single client)

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { 
        perror("socket"); 
        return 1; 
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { 
        perror("bind"); 
        return 1; 
    }
    if (listen(server_fd, 1) < 0) { 
        perror("listen"); 
        return 1; 
    }

    std::cout << "A: Listening on port " << listen_port << " for S3...\n";

    sockaddr_in cli{};
    socklen_t cl = sizeof(cli);
    g_client_fd = accept(server_fd, (sockaddr*)&cli, &cl);
    
    if (g_client_fd < 0) { 
        perror("accept"); 
        return 1; 
    }

    std::cout << "A: S3 connected.\n";

    auto start_p = std::chrono::steady_clock::now();

    pid_t pid1 = fork();

    if (pid1 < 0) { 
        perror("fork1"); 
        exit(1); 
    }
    if (pid1 == 0) {
        S1_smoothen(input_image);
        
        // cleanup
        munmap(shm_s1_s2, g_shm_size);
        sem_close(sem_s1s2_empty); sem_close(sem_s1s2_full);

        _exit(0);
    }

    // fork S2
    pid_t pid2 = fork();

    if (pid2 < 0) { 
        perror("fork2"); 
        exit(1); 
    }
    if (pid2 == 0) {
        S2_find_details(input_image);

        //cleanup
        munmap(shm_s1_s2, g_shm_size);
        sem_close(sem_s1s2_empty); sem_close(sem_s1s2_full);

        // close client socket 
        if (g_client_fd >= 0) 
            close(g_client_fd);

        _exit(0);
    }

    // parent
    munmap(shm_s1_s2, g_shm_size);

    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);

    auto finish_p = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = finish_p - start_p;
    std::cout << "Total Processing time : " << elapsed.count()*1000 << " ms\n";

    // unlink and close semaphores,servers
    sem_close(sem_s1s2_empty); 
    sem_close(sem_s1s2_full);

    sem_unlink(SEM_S1S2_EMPTY);
    sem_unlink(SEM_S1S2_FULL);

    if (g_client_fd >= 0) 
        close(g_client_fd);
    close(server_fd);

    shm_unlink(SHM_S1_S2_NAME);

    return 0;
}