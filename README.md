# High-Performance C++ OrderBook (DevLog)


## Build Instructions (macOS / Apple Silicon)
This project is natively compiled for ARM64 architecture.

### Prerequisites
* CMake (3.15+)
* Intel TBB (`brew install tbb`)
* Apple Clang

### Compilation
Because Homebrew installs to `/opt/homebrew` on Apple Silicon, CMake is configured to route specifically to these directories.

```bash
mkdir build && cd build
cmake ..
make
./orderbook
```

---

## Architecture 
This section documents the specific engineering decisions I made to keep the engine fast and deterministic.

### 1. Memory Alignment & The `Order` Struct
The `Order` struct fits perfectly inside a single CPU Cache Line to prevent unaligned memory reads.
* **No Floats:** Floating-point math causes rounding errors and is slower for the CPU's ALU. I multiply all prices by 10,000 and store them as `uint64_t` ticks.
* **Diet Enums:** Standard enums take 4 bytes. I scoped my `Side` and `Type` enums to `uint8_t` to shrink them to 1 byte.
* **The 32-Byte Perfect Fit:** By ordering my variables carefully (`8 + 8 + 8 + 4 + 1 + 1 = 30`), the C++ compiler adds 2 bytes of invisible padding, aligning every single order exactly to a 32-byte boundary. Two complete orders fit flawlessly into my M2's 64-byte L1 cache line.

### 2. Enforcing Price-Time Priority
 
* **Price (The Red-Black Tree):** Used `std::map` to hold the prices. Finding the absolute best buyer or seller is always an $O(1)$ or $O(\log N)$ operation. 
* **Time (The Doubly Linked List):** The value inside the map is a `std::list<Order>`. New orders are pushed to the `.back()`, and the engine always executes against the `.front()`.

### 3. The Matching Loop & Memory Cleanup 
* **Crucial Detail:** I explicitly wrote logic to `.pop_front()` empty orders and `.erase()` empty price nodes from the map. 

### 4. $O(1)$ Order Cancellations

* **The Solution:** I implemented an "Active Order Tracker" using a `std::unordered_map`. When an order is placed, I use `std::prev(list.end())` to grab its exact memory location (its iterator) and store it in the hash map. 


---

