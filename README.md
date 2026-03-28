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

### 1. Order Storage
Each order is stored in a compact struct. Prices are handled as integer ticks instead of floats to avoid precision issues. The Side and Type fields use small enum types to keep the structure lightweight.

### 2. Price-Time Priority
Orders are grouped by price using std::map. For each price level, orders are kept in a std::list, so the oldest order at that price is matched first. This follows price-time priority.

### 3. Matching
When a new order comes in, the engine checks the best available opposite side price levels and matches orders one by one. After matching, empty orders and empty price levels are removed.

### 4. Fast Cancellation
To support cancellations, the book keeps a hash map of active order IDs and their positions in the order lists. This makes it quick to locate and remove an order.

---

