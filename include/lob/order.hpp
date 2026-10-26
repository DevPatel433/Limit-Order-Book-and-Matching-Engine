#pragma once

#include "lob/types.hpp"

namespace lob {

// An order as submitted and, for GTC limit orders, as stored in the book (where `quantity`
// is the *remaining* quantity). Members are ordered so the struct stays 32 bytes: the book
// allocates one list node and one hash node per resting order, so size matters
// (see docs/phase5_design.md).
struct Order {
    OrderId id{};
    Price price{};  // ignored for market orders
    Quantity quantity{};
    Side side{Side::Buy};
    TimeInForce tif{TimeInForce::GTC};
    OrderType type{OrderType::Limit};

    constexpr Order() = default;
    constexpr Order(OrderId id_, Side side_, Price price_, Quantity quantity_,
                    TimeInForce tif_ = TimeInForce::GTC, OrderType type_ = OrderType::Limit) noexcept
        : id(id_), price(price_), quantity(quantity_), side(side_), tif(tif_), type(type_) {}

    // Market order: no price limit. GTC is not valid for it (a market order cannot rest).
    static constexpr Order market(OrderId id, Side side, Quantity quantity,
                                  TimeInForce tif = TimeInForce::IOC) noexcept {
        return Order(id, side, 0, quantity, tif, OrderType::Market);
    }

    friend constexpr bool operator==(const Order&, const Order&) = default;
};
static_assert(sizeof(Order) == 32, "Order must stay 32 bytes (node size matters for the pooled allocator)");

// The worst price a taker will accept: its limit, or "anything" for a market order.
constexpr Price effective_limit(const Order& o) noexcept {
    if (o.type == OrderType::Market) return o.side == Side::Buy ? kMaxPrice : kMinPrice;
    return o.price;
}

}  // namespace lob
