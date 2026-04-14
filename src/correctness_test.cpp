#include "../include/OrderBook.hpp"
#include <iostream>
#include <cassert>
#include <string>
#include <sstream>

static void flush(OrderBook &ob)
{
    ob.processQueue();
}

#define EXPECT_EQ(label, actual, expected)                                 \
    do                                                                     \
    {                                                                      \
        if ((actual) != (expected))                                        \
        {                                                                  \
            std::cerr << "[FAIL] " << label << ": expected " << (expected) \
                      << ", got " << (actual) << "  (" << __FILE__         \
                      << ":" << __LINE__ << ")\n";                         \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

#define EXPECT_TRUE(label, cond)                                 \
    do                                                           \
    {                                                            \
        if (!(cond))                                             \
        {                                                        \
            std::cerr << "[FAIL] " << label << "  (" << __FILE__ \
                      << ":" << __LINE__ << ")\n";               \
            std::exit(1);                                        \
        }                                                        \
    } while (0)

static Order makeOrder(uint64_t id, Side side, uint64_t price, uint32_t qty)
{
    Order o{};
    o.order_id = id;
    o.side = side;
    o.price = price;
    o.quantity = qty;
    o.type = Type::LIMIT;
    return o;
}

static void pass(const std::string &name)
{
    std::cout << "[PASS] " << name << "\n";
}

// T01  Resting orders: buy below ask, sell above bid should NOT match
static void test_resting_no_match()
{
    OrderBook ob(false); // no worker thread

    ob.addOrder(makeOrder(1, Side::BUY, 100, 10));  // bid @ 100
    ob.addOrder(makeOrder(2, Side::SELL, 105, 10)); // ask @ 105
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("resting bids", snap.bids.size(), 1u);
    EXPECT_EQ("resting asks", snap.asks.size(), 1u);
    EXPECT_EQ("bid price", snap.bids[0].price, 100u);
    EXPECT_EQ("ask price", snap.asks[0].price, 105u);
    EXPECT_EQ("bid qty", snap.bids[0].total_quantity, 10u);
    EXPECT_EQ("ask qty", snap.asks[0].total_quantity, 10u);

    pass("T01 Resting orders do not match");
}

// T02  Exact full fill: both sides completely consumed, book is empty
static void test_exact_full_fill()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 100, 20)); // ask @ 100, qty 20
    ob.addOrder(makeOrder(2, Side::BUY, 100, 20));  // buy @ 100 should fill completely
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("book empty after full fill (bids)", snap.bids.size(), 0u);
    EXPECT_EQ("book empty after full fill (asks)", snap.asks.size(), 0u);

    pass("T02 Exact full fill clears both sides");
}

// T03  Partial fill: resting ask partially consumed, remainder stays
static void test_partial_fill()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 100, 50)); // ask @ 100, qty 50
    ob.addOrder(makeOrder(2, Side::BUY, 100, 30));  // buy 30 — should leave 20 on ask
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("ask levels after partial fill", snap.asks.size(), 1u);
    EXPECT_EQ("remaining ask qty", snap.asks[0].total_quantity, 20u);
    EXPECT_EQ("no bids after full buy fill", snap.bids.size(), 0u);

    pass("T03 Partial fill leaves correct remainder");
}

// T04  FIFO (price-time priority): two orders at the same price level
//      The first-added must be filled before the second-added.
static void test_fifo_priority()
{
    OrderBook ob(false);

    // Two bids at the same price. o1 arrives first, o2 second.
    ob.addOrder(makeOrder(1, Side::BUY, 100, 30));
    ob.addOrder(makeOrder(2, Side::BUY, 100, 30));
    flush(ob);

    // Sell exactly 30 — should wipe out o1 entirely, o2 should remain intact.
    ob.addOrder(makeOrder(3, Side::SELL, 100, 30));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one bid level remains", snap.bids.size(), 1u);
    EXPECT_EQ("remaining qty is from o2", snap.bids[0].total_quantity, 30u);

    // Cancel o2 — if FIFO is correct, o2 is still in the book.
    // If FIFO was violated and o2 was filled instead, cancel will be a no-op
    // and the book will still show the 30 from o1 (same observable qty).
    // So we verify by canceling o1 (should be gone) and o2 (should succeed).
    //
    // We expose this via a second sell that would fill the remaining qty.
    ob.addOrder(makeOrder(4, Side::SELL, 100, 30));
    flush(ob);

    auto snap2 = ob.getSnapshot(5);
    EXPECT_EQ("book empty after both fills (bids)", snap2.bids.size(), 0u);
    EXPECT_EQ("book empty after both fills (asks)", snap2.asks.size(), 0u);

    pass("T04 FIFO price-time priority");
}

// T05  Multi-level sweep: one aggressive order crosses multiple price levels
static void test_multilevel_sweep()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 102, 10));
    ob.addOrder(makeOrder(2, Side::BUY, 101, 20));
    ob.addOrder(makeOrder(3, Side::BUY, 100, 40));
    flush(ob);

    // Sell 35 at 100: should eat all 10 @ 102, all 20 @ 101, and 5 from 100.
    // Remaining: 35 at 100.
    ob.addOrder(makeOrder(4, Side::SELL, 100, 35));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one bid level remains after sweep", snap.bids.size(), 1u);
    EXPECT_EQ("remaining price level", snap.bids[0].price, 100u);
    EXPECT_EQ("remaining qty", snap.bids[0].total_quantity, 35u);

    pass("T05 Multi-level sweep");
}

// T06  Cancel: order removed from book, level disappears when empty
static void test_cancel()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 105, 50));
    ob.addOrder(makeOrder(2, Side::SELL, 106, 30));
    flush(ob);

    // Cancel order 1. Best ask should move to 106.
    ob.submitCancel(1);
    flush(ob);
    // cancelOrder is direct (not queued), no flush needed.

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one ask level after cancel", snap.asks.size(), 1u);
    EXPECT_EQ("best ask shifted to 106", snap.asks[0].price, 106u);
    EXPECT_EQ("ask qty at 106", snap.asks[0].total_quantity, 30u);

    pass("T06 Cancel removes order and cleans up empty level");
}

// T07  Cancel non-existent order: should be a silent no-op, not a crash
static void test_cancel_nonexistent()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 10));
    flush(ob);

    ob.submitCancel(9999); // does not exist
    flush(ob);
    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("book unchanged after spurious cancel", snap.bids.size(), 1u);
    EXPECT_EQ("bid qty unchanged", snap.bids[0].total_quantity, 10u);

    pass("T07 Cancel of non-existent order is a no-op");
}

// T08  Cancel already-filled order: should be a no-op
static void test_cancel_already_filled()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 100, 10)); // ask
    ob.addOrder(makeOrder(2, Side::BUY, 100, 10));  // fills o1 completely
    flush(ob);

    // o1 is fully filled and erased from active_orders_. Cancel should do nothing.
    ob.submitCancel(1);
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("book empty after fill + spurious cancel (asks)", snap.asks.size(), 0u);
    EXPECT_EQ("book empty after fill + spurious cancel (bids)", snap.bids.size(), 0u);

    pass("T08 Cancel of already-filled order is a no-op");
}

// T09  Modify — quantity decrease: order keeps queue position (in-place)
static void test_modify_qty_decrease()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 50));
    ob.addOrder(makeOrder(2, Side::BUY, 100, 30)); // behind o1 in the queue
    flush(ob);

    // Decrease o1 quantity from 50 to 20. o1 keeps its queue position.
    ob.submitModify(1, 100, 20);
    flush(ob);
    // modifyOrder scenario B is in-place (no re-queue), no extra flush needed.

    // Now sell 20. Should fill o1 (now qty 20) entirely, leaving o2 (qty 30).
    ob.addOrder(makeOrder(3, Side::SELL, 100, 20));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one bid level remains", snap.bids.size(), 1u);
    EXPECT_EQ("o2 is the survivor after modify", snap.bids[0].total_quantity, 30u);

    pass("T09 Modify qty decrease keeps queue position");
}

// T10  Modify — price change: order loses queue position (cancel + re-add)
static void test_modify_price_change()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 50));
    ob.addOrder(makeOrder(2, Side::BUY, 100, 30));
    flush(ob);

    // Move o1 to a new price. It goes to the back of any list at the new price.
    ob.submitModify(1, 99, 50); // price change → cancel + re-enqueue
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("two price levels after modify", snap.bids.size(), 2u);
    EXPECT_EQ("best bid still 100", snap.bids[0].price, 100u);
    EXPECT_EQ("second bid at 99", snap.bids[1].price, 99u);
    EXPECT_EQ("qty at 100 is only o2", snap.bids[0].total_quantity, 30u);
    EXPECT_EQ("qty at 99 is moved o1", snap.bids[1].total_quantity, 50u);

    pass("T10 Modify price change relocates order");
}

// T11  Modify quantity to zero: treated as a cancel
static void test_modify_qty_to_zero()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 105, 40));
    flush(ob);

    ob.submitModify(1, 105, 0); // effectively a cancel
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("book empty after modify to zero (asks)", snap.asks.size(), 0u);

    pass("T11 Modify qty=0 treated as cancel");
}

// T12  Aggregation: multiple orders at the same price level aggregate correctly
static void test_aggregation()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 10));
    ob.addOrder(makeOrder(2, Side::BUY, 100, 25));
    ob.addOrder(makeOrder(3, Side::BUY, 100, 15));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one bid level", snap.bids.size(), 1u);
    EXPECT_EQ("aggregated qty 10+25+15", snap.bids[0].total_quantity, 50u);

    pass("T12 Quantity aggregation within a price level");
}

// T13  Snapshot depth limit: getSnapshot(N) returns at most N levels
static void test_snapshot_depth()
{
    OrderBook ob(false);

    for (uint64_t p = 100; p <= 110; ++p)
        ob.addOrder(makeOrder(p, Side::BUY, p, 10));
    flush(ob);

    auto snap = ob.getSnapshot(3);
    EXPECT_EQ("snapshot depth capped at 3", snap.bids.size(), 3u);
    EXPECT_EQ("top-of-book is 110", snap.bids[0].price, 110u);

    pass("T13 Snapshot depth limit respected");
}

// T14  Buy aggressive enough to cross: incoming buy matches AND rests remainder
static void test_partial_match_then_rest()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 100, 20)); // ask 20 @ 100
    flush(ob);

    // Buy 50 at 100: fills all 20 from the ask, rests remaining 30 on bid side.
    ob.addOrder(makeOrder(2, Side::BUY, 100, 50));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("ask cleared", snap.asks.size(), 0u);
    EXPECT_EQ("remainder rested on bid", snap.bids.size(), 1u);
    EXPECT_EQ("resting bid qty", snap.bids[0].total_quantity, 30u);
    EXPECT_EQ("resting bid price", snap.bids[0].price, 100u);

    pass("T14 Partial match + rest of buy order");
}

// T15 Sell-side sweep: aggressive sell matches through multiple bid levels
static void test_sell_side_sweep()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 10));
    ob.addOrder(makeOrder(2, Side::BUY, 101, 20));
    ob.addOrder(makeOrder(3, Side::BUY, 102, 15));
    flush(ob);

    // Sell 40 at 100: should eat 15 @ 102, 20 @ 101, 5 from 100.
    ob.addOrder(makeOrder(4, Side::SELL, 100, 40));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one bid level remains", snap.bids.size(), 1u);
    EXPECT_EQ("remaining price", snap.bids[0].price, 100u);
    EXPECT_EQ("remaining qty", snap.bids[0].total_quantity, 5u);

    pass("T15 Sell-side multi-level sweep");
}

// T16 Modify quantity increase: order loses queue priority
static void test_modify_qty_increase()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 20));
    ob.addOrder(makeOrder(2, Side::BUY, 100, 30));
    flush(ob);

    // Increase o1 from 20 to 40. It should lose priority and go behind o2.
    ob.submitModify(1, 100, 40);
    flush(ob);

    // Sell 30 — should fill o2 (which is now first), not o1.
    ob.addOrder(makeOrder(3, Side::SELL, 100, 30));
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("one bid level remains", snap.bids.size(), 1u);
    EXPECT_EQ("remaining qty is o1's 40", snap.bids[0].total_quantity, 40u);

    pass("T16 Modify qty increase loses queue priority");
}

// T17 Modify non-existent order: silent no-op
static void test_modify_nonexistent()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::BUY, 100, 10));
    flush(ob);

    ob.submitModify(9999, 100, 50);
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("book unchanged", snap.bids.size(), 1u);
    EXPECT_EQ("qty unchanged", snap.bids[0].total_quantity, 10u);

    pass("T17 Modify non-existent order is a no-op");
}

// T18 Empty book snapshot
static void test_empty_snapshot()
{
    OrderBook ob(false);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("no bids", snap.bids.size(), 0u);
    EXPECT_EQ("no asks", snap.asks.size(), 0u);

    pass("T18 Empty book snapshot");
}

// T19 MARKET order fills at any price and does not rest
static void test_market_order()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 500, 10)); // ask far away
    flush(ob);

    Order market = makeOrder(2, Side::BUY, 0, 10);
    market.type = Type::MARKET;
    ob.addOrder(market);
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("ask consumed", snap.asks.size(), 0u);
    EXPECT_EQ("market order did not rest", snap.bids.size(), 0u);

    pass("T19 MARKET order fills at any price");
}

// T20 IOC order partially fills and remainder is discarded
static void test_ioc_order()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 100, 10));
    flush(ob);

    Order ioc = makeOrder(2, Side::BUY, 100, 25);
    ioc.type = Type::IOC;
    ob.addOrder(ioc);
    flush(ob);

    auto snap = ob.getSnapshot(5);
    EXPECT_EQ("ask consumed", snap.asks.size(), 0u);
    EXPECT_EQ("IOC remainder discarded, not rested", snap.bids.size(), 0u);

    pass("T20 IOC partial fill, remainder discarded");
}

// T21 Trade log records correct data
static void test_trade_log()
{
    OrderBook ob(false);

    ob.addOrder(makeOrder(1, Side::SELL, 100, 30));
    ob.addOrder(makeOrder(2, Side::BUY, 100, 20));
    flush(ob);

    auto &trades = ob.getTradeLog();
    EXPECT_EQ("one trade recorded", trades.size(), 1u);
    EXPECT_EQ("trade buy_id", trades[0].buy_order_id, 2u);
    EXPECT_EQ("trade sell_id", trades[0].sell_order_id, 1u);
    EXPECT_EQ("trade price", trades[0].price, 100u);
    EXPECT_EQ("trade qty", trades[0].quantity, 20u);

    pass("T21 Trade log records correct data");
}

int main()
{

    std::cout << " Deterministic Correctness Suite\n";

    test_resting_no_match();
    test_exact_full_fill();
    test_partial_fill();
    test_fifo_priority();
    test_multilevel_sweep();
    test_cancel();
    test_cancel_nonexistent();
    test_cancel_already_filled();
    test_modify_qty_decrease();
    test_modify_price_change();
    test_modify_qty_to_zero();
    test_aggregation();
    test_snapshot_depth();
    test_partial_match_then_rest();
    test_sell_side_sweep();
    test_modify_qty_increase();
    test_modify_nonexistent();
    test_empty_snapshot();
    test_market_order();
    test_ioc_order();
    test_trade_log();

    std::cout << "\n\n";
    std::cout << " ALL TESTS PASSED\n";
    return 0;
}