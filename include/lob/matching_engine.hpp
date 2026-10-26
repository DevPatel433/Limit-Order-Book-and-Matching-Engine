#pragma once

#include <vector>

#include "lob/events.hpp"
#include "lob/order.hpp"
#include "lob/order_book.hpp"

namespace lob {

// Continuous limit-order matching with price-time priority. The full rules, with
// examples, are in docs/matching_rules.md. In short:
//   * A buy crosses while the best ask <= its limit; a sell while best bid >= it.
//     A market order has no limit: it crosses everything.
//   * Execution is against the best opposing price first, FIFO within a price.
//   * Every trade executes at the *resting* (maker) order's price.
//   * Time in force decides the remainder: GTC rests at its limit price (back of the queue);
//     IOC and market orders discard it (Expired); FOK fills completely or not at all.
//   * amend() keeps time priority only if the price is unchanged and the quantity did not increase.
//   * No self-trade prevention: orders are anonymous.
//
// Single-threaded and deterministic: same call sequence -> same event sequence.
template <class Policy>
class BasicMatchingEngine {
public:
    // Process a new order. Events are APPENDED to `out` (not cleared),
    // so one buffer can be reused across calls without allocating.
    void submit(const Order& order, std::vector<Event>& out);
    // Request cancellation of a live order; same appending contract.
    void cancel(OrderId id, std::vector<Event>& out);
    // Change the price and/or OPEN quantity of a resting order (see Amended in events.hpp).
    void amend(OrderId id, Price new_price, Quantity new_quantity, std::vector<Event>& out);

    // Convenience overloads that return a fresh vector.
    std::vector<Event> submit(const Order& order);
    std::vector<Event> cancel(OrderId id);
    std::vector<Event> amend(OrderId id, Price new_price, Quantity new_quantity);

    const BasicOrderBook<Policy>& book() const noexcept { return book_; }

private:
    // Executes `taker` against the opposite side within its effective limit, then disposes of any
    // remainder: GTC rests, IOC/FOK/market expire. Emits Trade/Expired events (not Accepted).
    [[gnu::always_inline]] inline void match_and_dispose(const Order& taker, std::vector<Event>& out);

    BasicOrderBook<Policy> book_;
};

// Baseline (std allocator) and optimised (arena) implementations: identical behaviour.
using MatchingEngine = BasicMatchingEngine<StdPolicy>;
using PooledMatchingEngine = BasicMatchingEngine<PoolPolicy>;
extern template class BasicMatchingEngine<StdPolicy>;
extern template class BasicMatchingEngine<PoolPolicy>;

}  // namespace lob
