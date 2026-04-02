#include "OrderBook.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

int main()
{
    OrderBook engine;
    std::vector<uint64_t> ids;

    std::cout << "[TEST] Starting Modification Benchmark...\n";

    for (int i = 1; i <= 1000; ++i)
    {
        Order o;
        o.order_id = i;
        o.side = Side::BUY;
        o.price = 500;
        o.quantity = 100;
        engine.addOrder(o);
        ids.push_back(i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::cout << "[TEST] Executing 500 In-Place Quantity Decreases...\n";
    for (int i = 0; i < 500; ++i)
    {
        engine.modifyOrder(ids[i], 500, 50); // Same price, lower quantity
    }

    std::cout << "[TEST] Executing 500 Price-Change Cancel-Replaces...\n";
    for (int i = 500; i < 1000; ++i)
    {
        engine.modifyOrder(ids[i], 501, 100); // Price changed, triggers re-insert
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.printLatencyStats();

    return 0;
}