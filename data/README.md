# Benchmark Datasets

This directory contains the datasets used to benchmark the parallel Aho-Corasick implementation. To ensure realistic memory footprints and processing loads, we use industry-standard datasets for both the pattern dictionary and the target search corpus.

## 1. Pattern Dictionary: Snort 3 Community Rules
To simulate the state machine (DFA) of a real-world Intrusion Detection System (IDS), we extract literal string patterns from the Snort 3 Community Ruleset.

*   **Source:** [Snort Community Rules](https://www.snort.org/downloads/#rule-downloads)
*   **Processed File:** `patterns_snort.txt`
*   **Characteristics:** Contains over 4,000 exact-match signatures (hex and text) extracted from the `content:` fields of the `.rules` files. Regular expressions (PCRE) are excluded.
*   **Purpose:** Generates a massive state machine with thousands of states, ideal for testing the algorithm's performance when the DFA size exceeds typical L1/L2 cache boundaries, challenging the memory bandwidth and caching efficiency during parallel execution.

## 2. Target Corpus: Enron Email Dataset
To measure raw throughput (GB/s) and parallel speedup without being bottlenecked by disk I/O or network packet overhead, we use a massive contiguous text file.

*   **Source:** [CMU Enron Email Dataset](https://www.cs.cmu.edu/~./enron/)
*   **Processed File:** `enron_corpus.txt`
*   **Characteristics:** Approximately 1.4 GB of concatenated, unstructured text data compiled from ~500,000 emails.
*   **Purpose:** Provides a large enough dataset to map into shared memory (`mmap`). This ensures the execution time is heavily dominated by CPU pattern-matching rather than thread creation overhead, allowing for an accurate calculation of the parallel speedup curve.

## Reproducibility
The data files in this directory are generated automatically. If they are missing, you can download and prepare them by running:

```bash
../scripts/prepare_datasets.sh
```
