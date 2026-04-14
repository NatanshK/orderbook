# Limit Order Book Matching Engine (C++17)

A low-latency order book matching engine built from scratch in C++17 with a TCP network gateway. Supports LIMIT, MARKET, and IOC order types, cancel/modify operations, and real-time market data snapshots over a binary protocol.


## What it does

- Matches incoming buy and sell orders using strict **price-time priority** (FIFO within each price level)
- Supports **LIMIT**, **MARKET**, and **IOC** (Immediate-or-Cancel) order types
- Handles partial fills, multi-level sweeps, order cancellation, and order modification with correct queue priority semantics
- Ingests orders over raw **TCP sockets** using macOS `kqueue` for non-blocking I/O multiplexing
- Serves real-time **market data snapshots** (top-of-book depth) as a packed binary protocol
- Logs every trade with buyer/seller IDs, price, quantity, and nanosecond timestamps
- Includes a **21-test deterministic correctness suite** and a **TCP stress test** that fires 100K orders

## Performance

Tested on Apple M2, compiled with `-O3 -mcpu=apple-m2`.

Stress test: 100,000 orders over TCP, alternating buys and sells at the same price so every order triggers a match.

|Metric |Value |
|---------------|---------|
|Average latency|~1,000 ns|
|Min latency |83 ns |
|99th percentile|~24 µs |
|Max latency |~106 µs |



## Architecture

### Core data structures

The book uses `std::map<price, std::list<Order>>` — a red-black tree keyed by price, with a doubly linked list of orders at each level. This gives:

- **O(log n)** to find best bid/ask (tree traversal)
- **O(1)** to match the front order at a level (list pop)
- **O(1)** to cancel any order anywhere in the book

That last one works because of a `tbb::concurrent_hash_map<order_id, list::iterator>` that stores a direct iterator to every resting order. When a cancel comes in, we look up the iterator, jump straight to the node, and erase it — no tree traversal needed.

### Concurrency model 

The network layer and the matching engine run on separate threads. The TCP server parses incoming commands and pushes `Order` structs (tagged with an `Action:` ADD, CANCEL, or MODIFY) into a `tbb::concurrent_queue`. A single dedicated worker thread pops from the queue and is the **only thread that ever mutates the order book**.

This eliminates lock contention on the hot matching path. The only mutexes in the system are `std::shared_mutex` on each side of the book (bids/asks), and they exist solely to let `getSnapshot()` read consistently from the network thread while the worker is writing.


### Order modification rules

These follow real exchange semantics:

- **Quantity decrease** → in-place update; the order keeps its original timestamp and queue position
- **Quantity increase** → cancel + re-add at the back of the queue (you’re asking for more, so you lose priority)
- **Price change** → cancel + re-add (fundamentally a new order)
- **Quantity to zero** → treated as a cancel

### Order types

- **LIMIT**: Match if the price crosses, otherwise rest in the book
- **MARKET**: Match at any available price. Never rests — if there’s nothing to match against, the remainder is discarded
- **IOC**: Match what you can immediately, discard the rest. Same as MARKET but respects the price limit

### Network layer

The TCP server uses macOS `kqueue` for event-driven I/O multiplexing — one thread handles many clients without spawning a thread per connection. Incoming bytes are buffered per client and split on newline boundaries, with safe handling of fragmented TCP packets and malformed input.

The text protocol looks like:

```
ADD <order_id> <BUY|SELL> <price> <quantity> [MARKET|IOC]
CAN <order_id>
MOD <order_id> <new_price> <new_quantity>
VIEW
SHUTDOWN
```

`VIEW` returns a packed binary snapshot (not text) — a 9-byte header followed by 12-byte level entries. The Python client (`client.py`) demonstrates how to decode it.

## Project structure

```
include/
Order.hpp — Order, Trade, Side, Type, Action structs
OrderBook.hpp — OrderBook class with queue-based concurrency
TCPServer.hpp — kqueue-based TCP server

src/
OrderBook.cpp — Matching engine, cancel/modify, snapshots, latency stats
TCPServer.cpp — Network parsing, binary snapshot serialization
main.cpp — Wires up OrderBook + TCPServer on port 8080
correctness_test.cpp — 21 deterministic tests (no worker thread, manual queue flush)
stress_test.cpp — TCP client that blasts 100K orders and measures round-trip time

client.py — Python client that decodes binary VIEW snapshots
```

## Build and run

### Prerequisites

- macOS (uses `kqueue` for I/O multiplexing)
- CMake 3.15+
- Intel TBB (`brew install tbb`)
- Apple Clang (ships with Xcode Command Line Tools)

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

### View the order book from Python

With the server running:

```bash
python3 client.py
```

## Tests

The correctness suite has 21 tests covering:

- Resting orders that shouldn’t match (spread too wide)
- Exact full fills and partial fills
- FIFO price-time priority within a level
- Multi-level sweeps (one aggressive order eating through several price levels)
- Cancel: basic, non-existent order, already-filled order
- Modify: quantity decrease (keeps priority), quantity increase (loses priority), price change (relocates), quantity-to-zero (treated as cancel), non-existent order
- Aggregation of multiple orders at the same price level
- Snapshot depth limiting
- Partial match + rest (order crosses, fills partially, remainder rests on the other side)
- MARKET orders filling at any price and not resting
- IOC orders discarding unfilled remainder
- Trade log recording correct buyer/seller IDs, price, and quantity

All tests run synchronously using `OrderBook(false)` which disables the background worker thread, allowing the test to call `processQueue()` manually. This makes the tests fully deterministic with no timing dependencies.

