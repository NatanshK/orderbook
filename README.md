# High-Frequency Limit Order Book (C++17)

A highly optimized, low-latency Limit Order Book (LOB) matching engine built entirely in C++17.

I built this project to explore systems programming, concurrency, and operating system-level networking. It implements strict price-time priority matching and supports partial fills, cancellations, and real-time order ingestion over a raw TCP socket.

## Performance Benchmarks

*Tested on an Apple Silicon M2 processor.*

* **Internal Core Throughput:** 12.5 million operations/second (80 nanoseconds/order)
* **Network TCP Throughput:** 1.9 million messages/second (batched)
* **99th Percentile Latency:** 31.5 microseconds total round-trip, including network parsing and memory allocation

## Build Instructions

### macOS / Apple Silicon

This project is compiled natively for ARM64.

### Prerequisites

* CMake 3.15+
* Intel TBB (`brew install tbb`)
* Apple Clang

### Build

Homebrew installs libraries under `/opt/homebrew` on Apple Silicon, so CMake is configured to use those paths.

```bash
mkdir build
cd build
cmake ..
make
./orderbook
```

## Architecture and Design Overview

### 1. Network Multiplexing with `kqueue`

Instead of creating a thread per client, the server uses macOS `kqueue` to handle multiple TCP connections through a single event loop. This keeps the networking layer efficient, scalable, and non-blocking.

### 2. Lock-Free Handoff with a Single-Writer Model

Standard `std::mutex` locks cause OS context switches, which are expensive at microsecond latency. To avoid lock contention, the engine separates networking from book mutation:

* Network threads parse incoming orders and push them into an Intel TBB `concurrent_queue`
* A single dedicated background thread pops from the queue and mutates the order book

Because only one thread ever touches the Red-Black tree, the core matching path requires no locks.

### 3. Price-Time Priority Storage

Orders are grouped by price using a `std::map`, which keeps the spread sorted and provides `O(log n)` price lookup. Inside each price level, orders are stored in a `std::list`, ensuring matching against the oldest order at that price is an `O(1)` operation.

### 4. Fast Cancellations

The engine maintains a `tbb::concurrent_hash_map` of active Order IDs, storing direct iterators to each order’s exact position in the nested lists. This allows canceled orders to be removed from the book in `O(1)` time without traversing the Red-Black tree.

### 5. Defensive Parsing

High-throughput network testing revealed the "Poison Pill" packet issue, where malformed strings or fragmented buffers could crash the parser. The TCP handler now buffers raw bytes, safely splits messages on newline boundaries, and wraps integer parsing in a `try/catch` block. Malformed network strings are dropped safely instead of crashing the main thread.

## Testing

A custom TCP stress test client is included to overwhelm the OS network stack and measure the engine’s real latency.

### Run

Terminal 1:

```bash
./orderbook
```

Terminal 2:

```bash
./stress_test
```

The client batches 100,000 orders over localhost to maximize TCP throughput, waits for a shutdown acknowledgment, and then the engine prints its internal latency percentiles.
