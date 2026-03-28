#pragma once
#include <map>
#include <list>
#include "Order.hpp"

class OrderBook
{
private:
    std::map<uint64_t, std::list<Order>> asks_; // THE ASK BOOK (Sellers)
    // Using doubly linked list to allow efficient insertion and deletion of orders at the same price level.

    std::map<uint64_t, std::list<Order>, std::greater<uint64_t>> bids_; // THE BID BOOK (Buyers)

    std::unordered_map<uint64_t, std::list<Order>::iterator> active_orders_; // Map to track active orders for quick cancellation and modification.

public:
    OrderBook() = default;
    ~OrderBook() = default;

    void addOrder(Order order);
    void cancelOrder(uint64_t order_id);

private:
    void matchBuyOrder(Order &order);
    void matchSellOrder(Order &order);
};