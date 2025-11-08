
INCLUDES = -I includes
SUPPORTING_FILES = includes/libppm.cpp includes/rowPacket.cpp

INPUT= input_images/5.ppm
OUTPUT_PATH= output_images

# part1

part1: out/part1_out.* $(INPUT)
	mkdir -p output_images
	out/part1_out $(INPUT) $(OUTPUT_PATH)/output_part1.ppm 

out/part1_out.*: Part1/part1.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part1/part1.cpp $(SUPPORTING_FILES) -o out/part1_out


# part2

part2_1: out/part2_1_out.* $(INPUT)
	mkdir -p output_images
	out/part2_1_out $(INPUT) $(OUTPUT_PATH)/output_part2_1.ppm 

out/part2_1_out.*: Part2/part2_1.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part2/part2_1.cpp $(SUPPORTING_FILES) -o out/part2_1_out


part2_2: out/part2_2_out.* $(INPUT)
	mkdir -p output_images
	out/part2_2_out $(INPUT) $(OUTPUT_PATH)/output_part2_2.ppm 

out/part2_2_out.*: Part2/part2_2.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part2/part2_2.cpp $(SUPPORTING_FILES) -o out/part2_2_out


part2_3: out/part2_3_out.* $(INPUT)
	mkdir -p output_images
	out/part2_3_out $(INPUT) $(OUTPUT_PATH)/output_part2_3.ppm 

out/part2_3_out.*: Part2/part2_3.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part2/part2_3.cpp $(SUPPORTING_FILES) -o out/part2_3_out
	
# part 3

part3_1_A: out/part3_1_A_out.* $(INPUT)
	mkdir -p output_images
	out/part3_1_A_out $(INPUT) $(OUTPUT_PATH)/output_part3_1_A.ppm 

out/part3_1_A_out.*: Part3/part3_1_A.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part3/part3_1_A.cpp $(SUPPORTING_FILES) -o out/part3_1_A_out


part3_1_B: out/part3_1_B_out.* $(INPUT)
	mkdir -p output_images
	out/part3_1_B_out $(INPUT) $(OUTPUT_PATH)/output_part3_1_B.ppm 

out/part3_1_B_out.*: Part3/part3_1_B.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part3/part3_1_B.cpp $(SUPPORTING_FILES) -o out/part3_1_B_out

part3_2_A: out/part3_2_A_out.* $(INPUT)
	mkdir -p output_images
	out/part3_2_A_out $(INPUT) $(OUTPUT_PATH)/output_part3_2_A.ppm 

out/part3_2_A_out.*: Part3/part3_2_A.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part3/part3_2_A.cpp $(SUPPORTING_FILES) -o out/part3_2_A_out


part3_2_B: out/part3_2_B_out.* $(INPUT)
	mkdir -p output_images
	out/part3_2_B_out $(INPUT) $(OUTPUT_PATH)/output_part3_2_B.ppm 

out/part3_2_B_out.*: Part3/part3_2_B.cpp $(SUPPORTING_FILES)
	mkdir -p out
	g++ $(INCLUDES) Part3/part3_2_B.cpp $(SUPPORTING_FILES) -o out/part3_2_B_out

clean:
	rm -Force out/*.exe
	rm -Force out/*.out
	rm -Force out/*.o