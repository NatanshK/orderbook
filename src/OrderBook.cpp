#include "OrderBook.hpp"
#include <iostream>
#include <algorithm>

void OrderBook::addOrder(Order order)
{
    if (order.side == Side::BUY)
    {
        matchBuyOrder(order);
    }
    else
    {
        matchSellOrder(order);
    }
}

void OrderBook::matchBuyOrder(Order &order)
{
    // 1. Are there sellers?
    // 2. Is the cheapest seller asking for a price <= our buyer's limit price?
    // 3. Does our buyer still need shares?
    while (!asks_.empty() && asks_.begin()->first <= order.price && order.quantity > 0)
    {

        auto &best_price_list = asks_.begin()->second;
        Order &best_ask = best_price_list.front();

        uint32_t trade_qty = std::min(order.quantity, best_ask.quantity);

        std::cout << "[TRADE EXECUTED] " << trade_qty << " shares bought at " << asks_.begin()->first << " ticks.\n";

        // Deduct the quantities
        order.quantity -= trade_qty;
        best_ask.quantity -= trade_qty;

        // Cleanup empty orders and empty price levels
        if (best_ask.quantity == 0)
        {
            best_price_list.pop_front();
        }
        if (best_price_list.empty())
        {
            asks_.erase(asks_.begin());
        }
    }

    // If the buyer still needs shares, put the remainder in the Bid book
    if (order.quantity > 0)
    {
        bids_[order.price].push_back(order);

        auto new_order_iterator = std::prev(bids_[order.price].end());

        active_orders_[order.order_id] = new_order_iterator;

        std::cout << "[ORDER RESTING] BUY " << order.quantity << " shares resting at " << order.price << " ticks.\n";
    }
}

void OrderBook::matchSellOrder(Order &order)
{
    // 1. Are there buyers?
    // 2. Is the highest buyer offering a price >= our seller's limit price?
    // 3. Does our seller still need to offload shares?
    while (!bids_.empty() && bids_.begin()->first >= order.price && order.quantity > 0)
    {

        auto &best_price_list = bids_.begin()->second;
        Order &best_bid = best_price_list.front();

        uint32_t trade_qty = std::min(order.quantity, best_bid.quantity);

        std::cout << "[TRADE EXECUTED] " << trade_qty << " shares sold at " << bids_.begin()->first << " ticks.\n";

        order.quantity -= trade_qty;
        best_bid.quantity -= trade_qty;

        // Cleanup empty orders and empty price levels
        if (best_bid.quantity == 0)
        {
            best_price_list.pop_front();
        }
        if (best_price_list.empty())
        {
            bids_.erase(bids_.begin());
        }
    }

    // If the seller still has shares to offload, put the remainder in the Ask book
    if (order.quantity > 0)
    {
        asks_[order.price].push_back(order);

        auto new_order_iterator = std::prev(asks_[order.price].end());

        active_orders_[order.order_id] = new_order_iterator;

        std::cout << "[ORDER RESTING] SELL " << order.quantity << " shares resting at " << order.price << " ticks.\n";
    }
}

void OrderBook::cancelOrder(uint64_t order_id)
{

    auto tracker_it = active_orders_.find(order_id);

    if (tracker_it == active_orders_.end())
    {
        return;
    }

    std::list<Order>::iterator order_location = tracker_it->second;

    // We need to save the side and price before we destroy the order
    Side order_side = order_location->side;
    uint64_t order_price = order_location->price;

    std::cout << "[ORDER CANCELLED] ID: " << order_id << " at price " << order_price << " ticks.\n";

    if (order_side == Side::BUY)
    {
        bids_[order_price].erase(order_location);

        // Memory Cleanup: If the list is now empty, delete the map node
        if (bids_[order_price].empty())
        {
            bids_.erase(order_price);
        }
    }
    else
    {
        asks_[order_price].erase(order_location);

        // Memory Cleanup: If the list is now empty, delete the map node
        if (asks_[order_price].empty())
        {
            asks_.erase(order_price);
        }
    }

    active_orders_.erase(order_id);
}