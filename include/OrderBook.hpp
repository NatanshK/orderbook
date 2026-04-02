#pragma once
#include <map>
#include <list>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_queue.h>
#include <thread>
#include <atomic>
#include <shared_mutex>
#include <tbb/concurrent_vector.h>
#include <numeric>
#include "Order.hpp"

class OrderBook
{
private:
    std::map<uint64_t, std::list<Order>> asks_; // THE ASK BOOK (Sellers)
    // Using doubly linked list to allow efficient insertion and deletion of orders at the same price level.

    std::map<uint64_t, std::list<Order>, std::greater<uint64_t>> bids_; // THE BID BOOK (Buyers)

    tbb::concurrent_hash_map<uint64_t, std::list<Order>::iterator> active_orders_; // Map to track active orders for quick cancellation and modification.

    mutable std::shared_mutex asks_mutex_;
    mutable std::shared_mutex bids_mutex_;

    tbb::concurrent_queue<Order> order_queue_; // Thread-safe queue for incoming orders to be processed by worker threads.

    std::atomic<bool> is_running_; // A thread-safe kill switch
    std::thread worker_thread_;    // The actual background process

    void workerLoop(); // The infinite loop function the thread will run

public:
    OrderBook();
    ~OrderBook();

    void addOrder(Order order);
    void cancelOrder(uint64_t order_id);
    void modifyOrder(uint64_t order_id, uint64_t new_price, uint32_t new_quantity);
    void processQueue();
    void printLatencyStats();

private:
    void matchBuyOrder(Order &order);
    void matchSellOrder(Order &order);
    tbb::concurrent_vector<uint64_t> execution_latencies_;
};