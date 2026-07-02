#!/usr/bin/env python3
import os
import sys
import random
import subprocess
import math

BLOCK_SIZE = 8 * 1024 * 1024
SIZE_GIB = float(os.environ.get("SIZE", "4"))
HOT_FRAC = float(os.environ.get("HOT_FRAC", "0.25"))
DENSITY_TARGET = float(os.environ.get("DENSITY", "0.05")) # 0.42 is impossible for matches/byte
SKEWS = [float(x) for x in os.environ.get("SKEWS", "1.0 0.5 0.25 0.1").split()]
SEED = int(os.environ.get("SEED", "1234"))

ACLAB = "build/aclab"
CORPUS_IN = "data/enron_corpus.txt"
PATTERNS = "data/patterns_snort.txt"

def count_matches(aclab_path, patterns_path, input_path):
    cmd = [aclab_path, "--patterns", patterns_path, "--input", input_path, "--searcher", "sequential", "--warmup", "0", "--iters", "1"]
    res = subprocess.run(cmd, capture_output=True, text=True, check=True)
    for line in res.stdout.splitlines():
        if line.startswith("sequential"):
            parts = line.split()
            return int(parts[2]), int(parts[7])
    return 0, 0

def inject(cold_data, patterns, num_injections):
    random.seed(SEED + num_injections) # deterministic per num_injections
    data = bytearray(cold_data)
    for _ in range(num_injections):
        pat = random.choice(patterns)
        min_len = math.ceil(len(pat) * 0.8)
        cut = random.randint(min_len, len(pat))
        prefix = pat[:cut]
        pos = random.randint(0, len(data) - len(prefix))
        data[pos:pos+len(prefix)] = prefix
    return data

def main():
    if not os.path.exists(ACLAB):
        print(f"Error: {ACLAB} not found. Please build first.")
        sys.exit(1)
    if not os.path.exists(CORPUS_IN) or not os.path.exists(PATTERNS):
        print(f"Error: Missing {CORPUS_IN} or {PATTERNS}.")
        sys.exit(1)
        
    random.seed(SEED)
    
    with open(PATTERNS, "rb") as f:
        patterns = f.read().splitlines()
        
    with open(CORPUS_IN, "rb") as f:
        cold_block = f.read(BLOCK_SIZE)
        if len(cold_block) < BLOCK_SIZE:
            cold_block = cold_block.ljust(BLOCK_SIZE, b' ')

    tmp_hot = "data/.tmp_hot_block"
    print("Finding hot block injection count to reach target density...")
    low, high = 0, 2000000
    best_data = cold_block
    best_diff = 1.0
    
    density = 0
    while True:
        data = inject(cold_block, patterns, high)
        with open(tmp_hot, "wb") as f: f.write(data)
        _, matches = count_matches(ACLAB, PATTERNS, tmp_hot)
        density = matches / BLOCK_SIZE
        print(f"  Upper bound check: {high} injections -> density {density:.4f}")
        if density >= DENSITY_TARGET:
            break
        high *= 2

    iters = 0
    while low <= high and iters < 20:
        mid = (low + high) // 2
        data = inject(cold_block, patterns, mid)
        with open(tmp_hot, "wb") as f: f.write(data)
        _, matches = count_matches(ACLAB, PATTERNS, tmp_hot)
        density = matches / BLOCK_SIZE
        
        diff = abs(density - DENSITY_TARGET)
        if diff < best_diff:
            best_diff = diff
            best_data = data
            
        print(f"  Bisect: {mid} injections -> density {density:.4f}")
        
        if abs(density - DENSITY_TARGET) <= 0.01:
            best_data = data
            break
        elif density < DENSITY_TARGET:
            low = mid + 1
        else:
            high = mid - 1
        iters += 1
        
    if os.path.exists(tmp_hot):
        os.remove(tmp_hot)
        
    hot_block = best_data
    
    total_blocks = int(SIZE_GIB * 1024 * 1024 * 1024 / BLOCK_SIZE)
    n_hot = int(total_blocks * HOT_FRAC)
    n_cold = total_blocks - n_hot
    
    print(f"Total blocks: {total_blocks}, Hot: {n_hot}, Cold: {n_cold}")
    
    # Track the global matches and bytes for self-check
    uniform_matches = None
    uniform_bytes = None

    for skew in SKEWS:
        if skew == 1.0:
            name = "uniform"
        elif skew == 0.25:
            name = "clustered"
        else:
            name = f"s{skew}"
            
        out_file = f"data/enron_skew_{name}.txt"
        
        if os.path.exists(out_file):
            print(f"Checking existing {out_file}...")
            b, m = count_matches(ACLAB, PATTERNS, out_file)
            if b == total_blocks * BLOCK_SIZE:
                print(f"  Skipping {out_file}, already exists and valid. Bytes: {b}, Matches: {m}")
                if uniform_matches is None:
                    uniform_matches = m
                    uniform_bytes = b
                elif uniform_matches != m or uniform_bytes != b:
                    print(f"  Error: Parity mismatch! Expected {uniform_matches} matches and {uniform_bytes} bytes.")
                    sys.exit(1)
                continue

        print(f"Generating {out_file} with SKEW={skew}...")
        
        blocks = []
        if skew == 1.0:
            counter = 0.0
            step = n_hot / total_blocks
            hot_placed = 0
            for i in range(total_blocks):
                counter += step
                if counter >= 1.0 and hot_placed < n_hot:
                    blocks.append(True)
                    hot_placed += 1
                    counter -= 1.0
                else:
                    blocks.append(False)
            while blocks.count(True) < n_hot:
                blocks[blocks.index(False)] = True
        else:
            cluster_len = max(n_hot, int(total_blocks * skew))
            counter = 0.0
            step = n_hot / cluster_len
            hot_placed = 0
            for i in range(cluster_len):
                counter += step
                if counter >= 1.0 and hot_placed < n_hot:
                    blocks.append(True)
                    hot_placed += 1
                    counter -= 1.0
                else:
                    blocks.append(False)
            while len(blocks) < total_blocks:
                blocks.append(False)
            while blocks.count(True) < n_hot:
                blocks[blocks.index(False)] = True
                
        with open(out_file, "wb") as f:
            for is_hot in blocks:
                f.write(hot_block if is_hot else cold_block)
                
        print(f"  Validating {out_file}...")
        b, m = count_matches(ACLAB, PATTERNS, out_file)
        print(f"  -> {b} bytes, {m} matches.")
        
        if uniform_matches is None:
            uniform_matches = m
            uniform_bytes = b
        else:
            if b != uniform_bytes or m != uniform_matches:
                print(f"Error: Parity mismatch! uniform matches={uniform_matches}, this={m}")
                sys.exit(1)

    print(f"Success! Global density: {uniform_matches / uniform_bytes:.4f}")

if __name__ == "__main__":
    main()
