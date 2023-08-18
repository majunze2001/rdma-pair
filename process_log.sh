#!/bin/bash

# Define the items to extract
items=("total_time" "lock_time" "ps_time" "ws_time" "pr_time" "wr_time" "unlock_time")

# For each item, extract the numbers, save them in separate files, and compute the average
for item in "${items[@]}"; do
    # Extract the numbers and save them in separate files
    awk -v item="$item" '$1 == item { print $2 }' timing_log_immediate.txt > "${item}.txt"
    # awk -v item="$item" '$1 == item { print $2 }' timing_log.txt > "${item}.txt"
    
    # Compute and print the average
    echo "${item} average: $(awk '{ sum += $1 } END { print sum / NR }' "${item}.txt")"
done
