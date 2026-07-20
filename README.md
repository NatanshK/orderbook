# Limit Order Book Matching Engine

A low-latency order book matching engine built from scratch in C++17 with a TCP network gateway. Supports LIMIT, MARKET, and IOC order types, cancel/modify operations, and real-time market data snapshots over a binary protocol.


## What it does

- Matches incoming buy and sell orders using strict **price-time priority** (FIFO within each price level)
- Supports **LIMIT**, **MARKET**, and **IOC** (Immediate-or-Cancel) order types
- Handles partial fills, multi-level sweeps, order cancellation, and order modification with correct queue priority semantics
- Ingests orders over raw **TCP sockets** using macOS `kqueue` for non-blocking I/O multiplexing
- Logs every trade with buyer/seller IDs, price, quantity, and nanosecond timestamps



## Performance

Tested on Apple M2, compiled with `-O3 -mcpu=apple-m2`.

The reported latency is **per-operation processing cost** measured inside the engine: the clock starts when the worker pops an order off the queue and stops when it finishes matching. It measures the engine's own work and deliberately excludes time spent waiting in the queue, so it stays stable regardless of load.

Measured over a multi-phase stress run (~174K operations including a deep resting book, cancels, modifies, concurrent mixed load, and a large multi-level sweep):

| Metric          | Value      |
|-----------------|------------|
| Average latency | ~195 ns    |
| 99th percentile | ~709 ns    |
| Max latency     | ~594 µs*   |



## Architecture

### Core data structures

The book uses `std::map<price, std::list<Order>>`, a red-black tree keyed by price, with a doubly linked list of orders at each level. This gives:

- **O(1)** to find the best bid/ask (best price is always at `begin()`; bids use a `std::greater` comparator so the highest bid is first, asks use the default so the lowest ask is first)
- **O(log n)** to insert or remove a price level (red-black tree operations)
- **O(1)** to match the front order at a level (list pop)
- **O(1)** to cancel any order anywhere in the book

That last one works because of a `tbb::concurrent_hash_map<order_id, list::iterator>` that stores a direct iterator to every resting order. When a cancel comes in, we look up the iterator, jump straight to the node, and erase it — no tree traversal needed. This relies on `std::list`'s **iterator stability**: a stored iterator stays valid across unrelated insertions and erasures, because list nodes never move.



## Project structure

```
include/
  Order.hpp          — Order, Trade, Side, Type, Action definitions
  OrderBook.hpp      — OrderBook class with queue-based concurrency
  TCPServer.hpp      — kqueue-based TCP server

src/
  OrderBook.cpp      — Matching engine, cancel/modify, snapshots, drain, latency stats
  TCPServer.cpp      — Network parsing, binary snapshot serialization
  main.cpp           — Wires up OrderBook + TCPServer on port 8080

correctness_test.cpp — 28 deterministic tests (no worker thread, manual queue flush)
stress_test.cpp      — Multi-phase concurrent TCP client
client.py            — Python client that decodes binary VIEW snapshots
```

## Build and run

### Prerequisites

- macOS (uses `kqueue` for I/O multiplexing)
- CMake 3.15+
- Intel TBB (`brew install tbb`)
- Apple Clang

### Build

```bash
mkdir build && cd build
cmake ..
make
```

### Run the server

```bash
./orderbook
```

### Run the correctness tests

```bash
./correctness_test
```

### Run the stress test

With the server running in another terminal:

```bash
./stress_test
```

The server prints a latency report (avg, min, max, p99) on shutdown.



