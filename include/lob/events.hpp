#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <variant>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

// Every state change the engine makes is described by exactly one of these, in
// the order it happened. Replaying the event stream reconstructs the book.

// A valid order was accepted. Emitted before any Trade it causes. Carries the
// order as submitted (original quantity).
struct Accepted {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};  // 0 for market orders
    Quantity quantity{};
    TimeInForce tif{TimeInForce::GTC};
    OrderType type{OrderType::Limit};
    friend bool operator==(const Accepted&, const Accepted&) = default;
};

enum class RejectReason : std::uint8_t {
    DuplicateId,
    InvalidPrice,
    InvalidQuantity,
    QuantityOverflow,
    UnknownOrder,  // cancel or amend of an ID that is not live
    InvalidTimeInForce,  // e.g. a GTC market order (it could never rest)
};
const char* to_string(RejectReason r) noexcept;
RejectReason to_reject_reason(AddStatus s) noexcept;  // s must not be Ok

// A submit or cancel was refused. The book is unchanged.
struct Rejected {
    OrderId id{};
    RejectReason reason{};
    friend bool operator==(const Rejected&, const Rejected&) = default;
};

// One execution between a resting (maker) order and an incoming (taker) order.
// `price` is always the maker's price. One Trade per maker order touched.
struct Trade {
    OrderId maker_id{};
    OrderId taker_id{};
    Side taker_side{Side::Buy};
    Price price{};
    Quantity quantity{};
    friend bool operator==(const Trade&, const Trade&) = default;
};

// A resting order was removed by a cancel request. `remaining` is the quantity
// that was still open (after any partial fills).
struct Cancelled {
    OrderId id{};
    Quantity remaining{};
    friend bool operator==(const Cancelled&, const Cancelled&) = default;
};

// The unfilled remainder of an IOC/FOK/market order was discarded (nothing rests). For FOK that is
// the whole quantity: it either fills completely or expires untouched. Always the last event for the order.
struct Expired {
    OrderId id{};
    Quantity remaining{};
    friend bool operator==(const Expired&, const Expired&) = default;
};

// A resting order was modified. Values are the OPEN quantity before and after.
// keeps_priority is true iff the price is unchanged and the quantity did not increase: the order is
// reduced in place and keeps its queue position. Otherwise it loses time priority: it is re-queued
// at the back of its (possibly new) price level, and if the new price crosses the book Trade
// events follow with this order as the taker.
struct Amended {
    OrderId id{};
    Price old_price{};
    Quantity old_quantity{};
    Price new_price{};
    Quantity new_quantity{};
    bool keeps_priority{};
    friend bool operator==(const Amended&, const Amended&) = default;
};

using Event = std::variant<Accepted, Rejected, Trade, Cancelled, Expired, Amended>;

std::ostream& operator<<(std::ostream&, const Accepted&);
std::ostream& operator<<(std::ostream&, const Rejected&);
std::ostream& operator<<(std::ostream&, const Trade&);
std::ostream& operator<<(std::ostream&, const Cancelled&);
std::ostream& operator<<(std::ostream&, const Expired&);
std::ostream& operator<<(std::ostream&, const Amended&);

// Single-line, stable text form, e.g. "TRADE maker=1 taker=2 BUY 100 x 5".
std::string to_string(const Event& e);

// CSV form of the event stream (see docs/replay_format.md). `seq` numbers events from 1, `cmd` is the
// 1-based index of the command that produced the event.
const char* csv_header() noexcept;
std::string to_csv_row(std::size_t seq, std::size_t cmd, const Event& e);

}  // namespace lob
