#!/bin/bash

IMG_DIR="../images"
RUNS=5

make

for img in "$IMG_DIR"/*.ppm; do
    echo "Processing $(basename "$img")"
    
    sum_read=0
    sum_smooth=0
    sum_details=0
    sum_sharp=0
    sum_write=0
    
    for ((i=1; i<=RUNS; i++)); do
        out_file="out.ppm"
        
        output=$(./a.out "$img" "$out_file")
        
        read_time=$(echo "$output"   | grep "file read" | awk '{print $4}')
        smooth_time=$(echo "$output" | grep "smooth"    | awk '{print $3}')
        details_time=$(echo "$output"| grep "details"   | awk '{print $3}')
        sharp_time=$(echo "$output"  | grep "sharp"    | awk '{print $3}')
        write_time=$(echo "$output"  | grep "File write"| awk '{print $4}')

        
        sum_read=$(echo "$sum_read + $read_time" | bc)
        sum_smooth=$(echo "$sum_smooth + $smooth_time" | bc)
        sum_details=$(echo "$sum_details + $details_time" | bc)
        sum_sharp=$(echo "$sum_sharp + $sharp_time" | bc)
        sum_write=$(echo "$sum_write + $write_time" | bc)
    done
    
    avg_read=$(echo "scale=4; $sum_read / $RUNS" | bc)
    avg_smooth=$(echo "scale=4; $sum_smooth / $RUNS" | bc)
    avg_details=$(echo "scale=4; $sum_details / $RUNS" | bc)
    avg_sharp=$(echo "scale=4; $sum_sharp / $RUNS" | bc)
    avg_write=$(echo "scale=4; $sum_write / $RUNS" | bc)
    
    echo "Average timings for $(basename "$img"):"
    echo "  File read: $avg_read ms"
    echo "  Smoothen : $avg_smooth ms"
    echo "  Details  : $avg_details ms"
    echo "  Sharpen  : $avg_sharp ms"
    echo "  File write: $avg_write ms"
    echo

done

make clean