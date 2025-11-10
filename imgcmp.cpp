#include <iostream>
#include "include/libppm.h"
#include <sys/wait.h>


int main(int argc,char **argv){
	if(argc != 3) {
		std::cout << "usage: ./a.out <path-to-original-image> <path-to-transformed-image>" << std::endl;
		exit(0);
	}

	std::cout << "\nVerifying Image..." <<std::endl;
    std::cout << "----------------------------------------------------------------------------------------------------------" << std::endl;


	struct image_t * input_image1=read_ppm_file(argv[1]);
	struct image_t * input_image2=read_ppm_file(argv[2]);

	for(int i = 1; i<input_image1->height-1; i++)
		for(int j = 1; j<input_image1->width-1; j++)
			for(int k=0; k<3; k++)
				if(input_image1->image_pixels[i][j][k] != input_image2->image_pixels[i][j][k]){
					std::cout << "\nPixel corrupted at "<<"("<< i <<", " << j <<", " << k <<") " <<std::endl;
					
					// silent exit 
					std::cout << "Exiting...." << std::endl;
					_exit(0);
				}

	std::cout << "\nImages are identical (pixel-by-pixel comparison passed)\n" << std::endl;

	return 0;
}