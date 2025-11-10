
INCLUDES = -I include
SUPPORTING_FILES = include/libppm.cpp include/rowPacket.cpp

INPUT = input_images/1.ppm

OUT_IMG_PATH = output_images
BIN_PATH = bin

# for part3
# 127.0.0.1 local
# 10.200.250.49 rahul
IP = 127.0.0.1
PORT = 9090

default:
	@echo "---------------------------------------------------------------------------------------------------------"
	@echo "Targets : "
	@echo "    1.part1"
	@echo "    2.part2_1"
	@echo "    3.part2_2"
	@echo "    4.part2_3"
	@echo "    5.part3_1_A"
	@echo "    6.part3_1_B"
	@echo "    7.part3_2_A"
	@echo "    8.part3_2_B"
	@echo "    9.check-part2_1"
	@echo "   10.check-part2_2"
	@echo "   11.check-part2_3"
	@echo "   12.check-part3_1"
	@echo "   13.check-part3_2"

# part1

part1 $(OUT_IMG_PATH)/output_part1.ppm: $(BIN_PATH)/part1_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part1_out $(INPUT) $(OUT_IMG_PATH)/output_part1.ppm 

$(BIN_PATH)/part1_out: Part1/part1.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part1/part1.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part1_out
	@echo
	@echo "Compiled part1,Exicuting ...."

# part2

part2_1 $(OUT_IMG_PATH)/output_part2_1.ppm: $(BIN_PATH)/part2_1_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part2_1_out $(INPUT) $(OUT_IMG_PATH)/output_part2_1.ppm 

$(BIN_PATH)/part2_1_out: Part2/part2_1/part2_1.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part2/part2_1/part2_1.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part2_1_out
	@echo
	@echo "Compiled part2_1,Exicuting ...."


part2_2 $(OUT_IMG_PATH)/output_part2_2.ppm: $(BIN_PATH)/part2_2_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part2_2_out $(INPUT) $(OUT_IMG_PATH)/output_part2_2.ppm 

$(BIN_PATH)/part2_2_out: Part2/part2_2/part2_2.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part2/part2_2/part2_2.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part2_2_out
	@echo
	@echo "Compiled part2_2,Exicuting ...."

part2_3 $(OUT_IMG_PATH)/output_part2_3.ppm: $(BIN_PATH)/part2_3_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part2_3_out $(INPUT) $(OUT_IMG_PATH)/output_part2_3.ppm 

$(BIN_PATH)/part2_3_out: Part2/part2_3/part2_3.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part2/part2_3/part2_3.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part2_3_out
	@echo
	@echo "Compiled part2_3,Exicuting ...."
	
# part 3

# 3_1
part3_1_A: $(BIN_PATH)/part3_1_A_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part3_1_A_out $(INPUT) $(PORT)

$(BIN_PATH)/part3_1_A_out: Part3/part3_1/part3_1_A.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part3/part3_1/part3_1_A.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part3_1_A_out 
	@echo
	@echo "Compiled part3_1_A,Exicuting ...."

part3_1_B: $(BIN_PATH)/part3_1_B_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part3_1_B_out $(INPUT) $(OUT_IMG_PATH)/output_part3_1.ppm $(IP) $(PORT)

$(BIN_PATH)/part3_1_B_out: Part3/part3_1/part3_1_B.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part3/part3_1/part3_1_B.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part3_1_B_out
	@echo
	@echo "Compiled part3_1_B,Exicuting ...."


# 3_2
part3_2_A: $(BIN_PATH)/part3_2_A_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part3_2_A_out $(INPUT) $(PORT)

$(BIN_PATH)/part3_2_A_out: Part3/part3_2/part3_2_A.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part3/part3_2/part3_2_A.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part3_2_A_out
	@echo
	@echo "Compiled part3_2_A,Exicuting ...."

part3_2_B: $(BIN_PATH)/part3_2_B_out $(INPUT)
	@ mkdir -p $(OUT_IMG_PATH)
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/part3_2_B_out $(INPUT) $(OUT_IMG_PATH)/output_part3_2.ppm $(IP) $(PORT)

$(BIN_PATH)/part3_2_B_out: Part3/part3_2/part3_2_B.cpp $(SUPPORTING_FILES)
	@ mkdir -p $(BIN_PATH)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) Part3/part3_2/part3_2_B.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/part3_2_B_out
	@echo
	@echo "Compiled part3_2_B,Exicuting ...."


# to Check output

$(BIN_PATH)/check_out: check.cpp $(SUPPORTING_FILES)

	@echo "---------------------------------------------------------------------------------------------------------"
	g++ $(INCLUDES) check.cpp $(SUPPORTING_FILES) -o $(BIN_PATH)/check_out
	@echo
	@echo "Compiled check.cpp,Exicuting ...."

#part2
check-part2_1: $(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part2_1.ppm
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part2_1.ppm

check-part2_2: $(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part2_2.ppm
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part2_2.ppm

check-part2_3: $(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part2_3.ppm
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part2_3.ppm


#part3

# 3_1
check-part3_1: $(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part3_1.ppm
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part3_1.ppm

# 3_2
check-part3_2: $(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part3_2.ppm
	@echo "---------------------------------------------------------------------------------------------------------"
	$(BIN_PATH)/check_out $(OUT_IMG_PATH)/output_part1.ppm $(OUT_IMG_PATH)/output_part3_2.ppm

clean:
	rm $(BIN_PATH)/*
	rm $(OUT_IMG_PATH)/*