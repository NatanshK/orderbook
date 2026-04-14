#include "OrderBook.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>

// CONSTRUCTOR
OrderBook::OrderBook() : is_running_(true)
{
    worker_thread_ = std::thread(&OrderBook::workerLoop, this);
    std::cout << "[SYSTEM] OrderBook Engine Started. Background Worker active.\n";
}

// DESTRUCTOR
OrderBook::~OrderBook()
{
    is_running_ = false;

    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }
    std::cout << "[SYSTEM] OrderBook Engine safely shut down.\n";
}

void OrderBook::addOrder(Order order)
{
    order.timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    order_queue_.push(order);
}

void OrderBook::processQueue()
{
    Order current_order;

    while (order_queue_.try_pop(current_order))
    {
        switch (current_order.action)
        {
        case Action::ADD:
            if (current_order.side == Side::BUY)
            {
                matchBuyOrder(current_order);
            }
            else
            {
                matchSellOrder(current_order);
            }
            break;

        case Action::CANCEL:
            cancelOrder(current_order.order_id);
            break;

        case Action::MODIFY:
            modifyOrder(current_order.order_id, current_order.price, current_order.quantity);
            break;
        }
    }
}

void OrderBook::workerLoop()
{
    while (is_running_)
    {

        processQueue();

        std::this_thread::yield();
    }
}

void OrderBook::matchBuyOrder(Order &order)
{
    {
        std::unique_lock<std::shared_mutex> asks_lock(asks_mutex_);

        while (!asks_.empty() && order.quantity > 0)
        {
            // LIMIT orders only match if ask price <= bid price
            if (order.type == Type::LIMIT && asks_.begin()->first > order.price)
                break;

            auto &best_price_list = asks_.begin()->second;
            Order &best_ask = best_price_list.front();

            uint32_t trade_qty = std::min(order.quantity, best_ask.quantity);

            auto end_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            execution_latencies_.push_back(end_time - order.timestamp);

            Trade t;
            t.buy_order_id = order.order_id;
            t.sell_order_id = best_ask.order_id;
            t.price = best_ask.price;
            t.quantity = trade_qty;
            t.timestamp = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            trade_log_.push_back(t);

            order.quantity -= trade_qty;
            best_ask.quantity -= trade_qty;

            if (best_ask.quantity == 0)
            {
                active_orders_.erase(best_ask.order_id);
                best_price_list.pop_front();
            }
            if (best_price_list.empty())
            {
                asks_.erase(asks_.begin());
            }
        }
    }
    if (order.quantity > 0 && order.type == Type::LIMIT)
    {
        std::unique_lock<std::shared_mutex> bids_lock(bids_mutex_);
        bids_[order.price].push_back(order);
        auto new_order_iterator = std::prev(bids_[order.price].end());
        active_orders_.insert({order.order_id, new_order_iterator});

        auto end_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        execution_latencies_.push_back(end_time - order.timestamp);
    }
}

void OrderBook::matchSellOrder(Order &order)
{
    {
        std::unique_lock<std::shared_mutex> bids_lock(bids_mutex_);

        while (!bids_.empty() && order.quantity > 0)
        {
            if (order.type == Type::LIMIT && bids_.begin()->first < order.price)
                break;

            auto &best_price_list = bids_.begin()->second;
            Order &best_bid = best_price_list.front();

            uint32_t trade_qty = std::min(order.quantity, best_bid.quantity);

            auto end_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            execution_latencies_.push_back(end_time - order.timestamp);

            Trade t;
            t.buy_order_id = best_bid.order_id;
            t.sell_order_id = order.order_id;
            t.price = best_bid.price;
            t.quantity = trade_qty;
            t.timestamp = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            trade_log_.push_back(t);

            order.quantity -= trade_qty;
            best_bid.quantity -= trade_qty;

            if (best_bid.quantity == 0)
            {
                active_orders_.erase(best_bid.order_id);
                best_price_list.pop_front();
            }
            if (best_price_list.empty())
            {
                bids_.erase(bids_.begin());
            }
        }
    }

    if (order.quantity > 0 && order.type == Type::LIMIT)
    {
        std::unique_lock<std::shared_mutex> asks_lock(asks_mutex_);
        asks_[order.price].push_back(order);
        auto new_order_iterator = std::prev(asks_[order.price].end());
        active_orders_.insert({order.order_id, new_order_iterator});

        auto end_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        execution_latencies_.push_back(end_time - order.timestamp);
    }
}

void OrderBook::cancelOrder(uint64_t order_id)
{
    tbb::concurrent_hash_map<uint64_t, std::list<Order>::iterator>::accessor a;

    if (!active_orders_.find(a, order_id))
    {
        return;
    }

    std::list<Order>::iterator order_location = a->second;
    Side order_side = order_location->side;
    uint64_t order_price = order_location->price;

    a.release();

    active_orders_.erase(order_id);

    // Acquiring an EXCLUSIVE lock for the specific side of the book and remove the node.
    if (order_side == Side::BUY)
    {
        std::unique_lock<std::shared_mutex> lock(bids_mutex_);

        auto price_level = bids_.find(order_price);
        if (price_level != bids_.end())
        {

            price_level->second.erase(order_location);

            if (price_level->second.empty())
            {
                bids_.erase(price_level);
            }
        }
    }
    else
    {
        std::unique_lock<std::shared_mutex> lock(asks_mutex_);

        auto price_level = asks_.find(order_price);
        if (price_level != asks_.end())
        {

            price_level->second.erase(order_location);

            if (price_level->second.empty())
            {
                asks_.erase(price_level);
            }
        }
    }
}

/*  Quantity Decrease? We lock the tree, shrink the size of the order via our iterator, and unlock. The trader keeps their place in line.

Quantity Increase? The trader is trying to jump the queue. We punish them by canceling their order and readding it to the back of the line with a new timestamp.

Price Change? This is a fundamentally new order. We cancel the old one and re-add it to the new price level.

Quantity becomes 0? This is just a cancellation in disguise.    */
void OrderBook::modifyOrder(uint64_t order_id, uint64_t new_price, uint32_t new_quantity)
{
    auto start_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    tbb::concurrent_hash_map<uint64_t, std::list<Order>::iterator>::accessor a;

    // Finding the order in the hash map
    if (!active_orders_.find(a, order_id))
    {
        return;
    }

    std::list<Order>::iterator order_location = a->second;
    Side order_side = order_location->side;
    uint64_t old_price = order_location->price;
    uint32_t old_quantity = order_location->quantity;

    // Releasing the hash map lock now prevents a self-deadlock.
    a.release();

    // Edge Case: Trader modified quantity to 0. Treat as a standard cancel.
    if (new_quantity == 0)
    {
        cancelOrder(order_id);
        return;
    }

    //  SCENARIO A: Loss of Queue Priority (Price changed OR Quantity increased)
    if (new_price != old_price || new_quantity > old_quantity)
    {

        // Constructing the replacement order
        Order updated_order;
        updated_order.order_id = order_id;
        updated_order.side = order_side;
        updated_order.price = new_price;
        updated_order.quantity = new_quantity;

        // Executing the Cancel-Replace
        cancelOrder(order_id);
        addOrder(updated_order);

        auto end_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        execution_latencies_.push_back(end_time - start_time);

        return;
    }

    // SCENARIO B: In-Place Modification (Price is identical AND Quantity decreased)
    // The trader gets to keep their original timestamp and place in the queue!
    if (new_quantity < old_quantity)
    {
        if (order_side == Side::BUY)
        {
            std::unique_lock<std::shared_mutex> lock(bids_mutex_);
            order_location->quantity = new_quantity;
        }
        else
        {
            std::unique_lock<std::shared_mutex> lock(asks_mutex_);
            order_location->quantity = new_quantity;
        }
        auto end_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        execution_latencies_.push_back(end_time - start_time);
    }
}

void OrderBook::submitCancel(uint64_t order_id)
{
    Order o{};
    o.order_id = order_id;
    o.action = Action::CANCEL;
    order_queue_.push(o);
}

void OrderBook::submitModify(uint64_t order_id, uint64_t new_price, uint32_t new_quantity)
{
    Order o{};
    o.order_id = order_id;
    o.price = new_price;
    o.quantity = new_quantity;
    o.action = Action::MODIFY;
    order_queue_.push(o);
}

void OrderBook::printLatencyStats()
{
    if (execution_latencies_.empty())
    {
        std::cout << "No trades executed or recorded.\n";
        return;
    }

    uint64_t sum = std::accumulate(execution_latencies_.begin(), execution_latencies_.end(), 0ULL);
    uint64_t avg = sum / execution_latencies_.size();

    std::vector<uint64_t> sorted_latencies(execution_latencies_.begin(), execution_latencies_.end());
    std::sort(sorted_latencies.begin(), sorted_latencies.end());

    uint64_t min = sorted_latencies.front();
    uint64_t max = sorted_latencies.back();
    uint64_t p99 = sorted_latencies[sorted_latencies.size() * 0.99];

    std::cout << "\n--- LATENCY REPORT ---\n";
    std::cout << "Total Operations : " << execution_latencies_.size() << "\n";
    std::cout << "Average        : " << avg << " ns\n";
    std::cout << "Min            : " << min << " ns\n";
    std::cout << "Max            : " << max << " ns\n";
    std::cout << "99th Pctl      : " << p99 << " ns\n";
    std::cout << "----------------------\n";
}

OrderBookSnapshot OrderBook::getSnapshot(int depth)
{
    OrderBookSnapshot snapshot;

    {
        std::shared_lock<std::shared_mutex> lock(bids_mutex_);
        int count = 0;
        for (auto const &[price, orders] : bids_)
        {
            if (count >= depth)
                break;
            uint32_t level_volume = 0;
            for (auto const &order : orders)
            {
                level_volume += order.quantity;
            }
            snapshot.bids.push_back({price, level_volume});
            count++;
        }
    }

    {
        std::shared_lock<std::shared_mutex> lock(asks_mutex_);
        int count = 0;
        for (auto const &[price, orders] : asks_)
        {
            if (count >= depth)
                break;
            uint32_t level_volume = 0;
            for (auto const &order : orders)
            {
                level_volume += order.quantity;
            }
            snapshot.asks.push_back({price, level_volume});
            count++;
        }
    }
    return snapshot;
}

OrderBook::OrderBook(bool start_worker) : is_running_(start_worker)
{
    if (start_worker)
        worker_thread_ = std::thread(&OrderBook::workerLoop, this);
}
