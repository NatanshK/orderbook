#pragma once
#include <cstdint>

// Using uint8_t (1 byte) instead of standard enums (which default to 4 bytes)
enum class Side : uint8_t
{
    BUY,
    SELL
};

enum class Type : uint8_t
{
    LIMIT,  // Rest in the book until the exact price is met
    MARKET, // Execute immediately at the best available price
    IOC,    // Immediate or Cancel - execute immediately or cancel the remaining quantity
    STOP    // Trigger a market order when the stop price is reached
};

struct Order
{
    uint64_t order_id;  // 8 bytes: Unique ID for the order
    uint64_t timestamp; // 8 bytes: Nanoseconds since epoch

    // Multiplying the price by 10,000 and storing it as a whole integer to avoid rounding errors.
    uint64_t price; // 8 bytes: Price in ticks

    uint32_t quantity; // 4 bytes: Number of shares/contracts
    Side side;         // 1 byte: BUY or SELL
    Type type;         // 1 byte: LIMIT, MARKET, etc.

    // 8 + 8 + 8 + 4 + 1 + 1 = 30 bytes.
    // The C++ compiler will automatically add 2 bytes of invisible "padding" at the end to align the struct perfectly to a 32-byte boundary!
};