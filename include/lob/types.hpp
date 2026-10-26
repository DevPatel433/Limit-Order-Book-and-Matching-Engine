#pragma once

#include <cstdint>
#include <limits>

namespace lob {

// Prices are integer ticks (never floating point). One tick is the smallest
// price increment; mapping ticks to currency is outside the engine's scope.
using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

// How long an order may live. GTC rests until filled or cancelled. IOC executes what it can
// immediately and discards the rest. FOK executes completely, immediately, or not at all.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

// Limit orders carry a price. Market orders have no price limit (they sweep the opposite side).
enum class OrderType : std::uint8_t { Limit, Market };

constexpr const char* to_string(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "?";
}

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr const char* to_string(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
}

// Valid prices are in [kMinPrice, kMaxPrice]. The upper bound is half of the
// int64 range so that (ask - bid) can never overflow.
constexpr Price kMinPrice = 1;
constexpr Price kMaxPrice = std::numeric_limits<Price>::max() / 2;

}  // namespace lob
