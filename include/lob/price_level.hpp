#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <limits>
#include <list>

#include "lob/order.hpp"
#include "lob/policy.hpp"

namespace lob {

// FIFO queue of resting orders at a single price.
//
// Backed by std::list so that every Handle (iterator) stays valid until its
// own element is erased. That is what lets the book's ID index cancel an order
// in O(1) without searching the queue. Node memory comes from Policy::Allocator.
template <class Policy>
class BasicPriceLevel {
public:
    using Allocator = typename Policy::template Allocator<Order>;
    using Queue = std::list<Order, Allocator>;
    using Handle = typename Queue::iterator;

    explicit BasicPriceLevel(Price price, const Allocator& alloc = Allocator()) : price_(price), orders_(alloc) {}

    Price price() const noexcept { return price_; }
    Quantity total_quantity() const noexcept { return total_quantity_; }
    std::size_t order_count() const noexcept { return orders_.size(); }
    bool empty() const noexcept { return orders_.empty(); }
    const Queue& orders() const noexcept { return orders_; }

    // True if adding `qty` would not overflow the level's aggregate quantity.
    bool can_accept(Quantity qty) const noexcept {
        return qty <= std::numeric_limits<Quantity>::max() - total_quantity_;
    }

    // Appends to the back of the queue (lowest time priority).
    // Preconditions: order.price == price(), order.quantity > 0, can_accept().
    Handle append(const Order& order) {
        assert(order.price == price_);
        assert(order.quantity > 0);
        assert(can_accept(order.quantity));
        total_quantity_ += order.quantity;
        orders_.push_back(order);
        return std::prev(orders_.end());
    }

    // Removes and returns the order with the highest time priority.
    Order pop_front() {
        assert(!orders_.empty());
        Order front = orders_.front();
        total_quantity_ -= front.quantity;
        orders_.pop_front();
        return front;
    }

    // Reduces the highest-priority order by `qty` (0 < qty <= its quantity).
    // The order keeps its queue position; if it is exhausted it is removed.
    // Returns true iff the order was removed.
    bool reduce_front(Quantity qty) {
        assert(!orders_.empty());
        Order& front = orders_.front();
        assert(qty > 0 && qty <= front.quantity);
        front.quantity -= qty;
        total_quantity_ -= qty;
        if (front.quantity != 0) return false;
        orders_.pop_front();
        return true;
    }

    // Lowers the quantity of the order at `handle` IN PLACE (0 < new_qty <= current), keeping its
    // queue position. This is how an amend that keeps time priority is applied.
    void reduce(Handle handle, Quantity new_qty) {
        assert(handle != orders_.end());
        assert(new_qty > 0 && new_qty <= handle->quantity);
        total_quantity_ -= handle->quantity - new_qty;
        handle->quantity = new_qty;
    }

    // Removes the order at `handle` from anywhere in the queue; O(1).
    Order erase(Handle handle) {
        assert(handle != orders_.end());
        Order removed = *handle;
        total_quantity_ -= removed.quantity;
        orders_.erase(handle);
        return removed;
    }

private:
    Price price_;
    Quantity total_quantity_{0};
    Queue orders_;
};

using PriceLevel = BasicPriceLevel<StdPolicy>;

}  // namespace lob
