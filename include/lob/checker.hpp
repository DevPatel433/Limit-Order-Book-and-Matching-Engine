#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/events.hpp"
#include "lob/matching_engine.hpp"

namespace lob {

// Independent verifier of an engine's behaviour, built only from the public
// event stream. It keeps its own "ledger" of live orders by applying events
// (event sourcing) and, after every command, checks:
//
//   per event   trade maker is live and on the opposite side; trade price is the
//               maker's price and respects the taker's limit; quantity never
//               exceeds either side's remaining; best price is taken first and
//               FIFO holds within a price; cancels report the true remaining;
//               duplicate/unknown rejections are justified.
//   per command the engine's book passes OrderBook::validate(); is not crossed;
//               snapshots are strictly sorted; and the book equals the ledger
//               order-for-order, including queue positions.
//
// Usage: construct on a FRESH engine, then call observe() with the events of
// each submit()/cancel() call, in order. After a violation is returned the
// checker's state is undefined; stop using it.
class ConsistencyChecker {
public:
    // Returns a description of the first violation, or nullopt if all is well.
    // Works with any engine implementation (baseline or pooled).
    template <class Policy>
    std::optional<std::string> observe(const std::vector<Event>& events,
                                       const BasicMatchingEngine<Policy>& engine);

private:
    struct Live {
        Order order;          // order.quantity is the remaining quantity
        std::uint64_t seq{};  // arrival order at the book
    };
    std::unordered_map<OrderId, Live> live_;
    std::uint64_t next_seq_{0};
};

}  // namespace lob
