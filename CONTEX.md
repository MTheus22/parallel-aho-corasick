
### Context and Prompt

**Project Context and General Objective**
The main objective is to implement and evaluate a parallel version of the search phase (matching phase) of the **Aho-Corasick** algorithm, aiming to maximize throughput when scanning large files on **multi-core CPU** architectures. The strategy utilizes **data parallelism** (intra-file parallelism) in shared memory, focusing on optimizing execution time and scalability without altering the mathematical functioning of the underlying automaton. The base parallelism model should use the **POSIX Threads (Pthreads)** API.

**Parallelization Strategy (Implementation Architecture)**
The implementation must be strictly divided into two phases: the pre-processing phase (sequential) and the scanning phase (highly parallel).

**1. Automaton Construction Phase (Sequential / Master Thread)**
*   **Action:** The master thread is responsible for constructing the Aho-Corasick structures (Trie tree, goto function, failure function, and output function) from a pattern dictionary.
*   **Memory Restriction:** Once the construction phase is finished, the automaton structure becomes **strictly read-only**. It must be allocated in shared memory, allowing multiple threads to access it simultaneously without the need for locking mechanisms (mutexes/locks) during the scan.

**2. Search / Scanning Phase (Parallel / Multiple Threads)**
The parallelization must apply the following principles:
*   **Data Partitioning (Chunking):** The large input text or file must be divided into fixed-size chunks. The master thread will logically divide the data buffer to distribute the workload.
*   **Edge Handling (Overlapping Chunks):** To prevent patterns from being cut and lost at the chunk boundaries, the chunks must have an overlap margin. The size of this overlap should generally be equal to the length of the longest pattern in the dictionary minus one byte (`max_pattern_length - 1`).
*   **Concurrent Execution:** A pool of slave threads will process the chunks independently and concurrently using the Pthreads API, all traversing the same automaton in shared memory.
*   **Result Isolation (Thread-Local Storage):** The use of single output files or shared global log structures during the search phase is prohibited, as locks would destroy performance. Each thread must store the found matches in a **private, thread-local match list**.
*   **Merging:** After the scanning is completed by all threads (`thread join`), the master thread collects the local lists from each thread and merges them to compile the final, unified search results.
