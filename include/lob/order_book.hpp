#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/order.hpp"
#include "lob/policy.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

enum class AddStatus : std::uint8_t {
    Ok,
    DuplicateId,
    InvalidPrice,
    InvalidQuantity,
    QuantityOverflow,
    InvalidTimeInForce,  // market order with GTC, or a non-GTC / market order passed to add()
};

const char* to_string(AddStatus s) noexcept;

// Aggregated view of one price level.
struct LevelSnapshot {
    Price price{};
    Quantity quantity{};
    std::size_t order_count{};

    friend bool operator==(const LevelSnapshot&, const LevelSnapshot&) = default;
};

// Bids are sorted best-first (descending price); asks best-first (ascending).
struct BookSnapshot {
    std::vector<LevelSnapshot> bids;
    std::vector<LevelSnapshot> asks;

    friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

// Two-sided book of resting limit orders.
//
// This class only *stores* orders. add() never matches, so a caller can build a
// crossed book (best bid >= best ask). MatchingEngine sits in front of it and
// guarantees that never happens; use the engine, not add(), for trading.
//
// Policy selects where node memory comes from (StdPolicy = baseline, PoolPolicy =
// arena). Behaviour is identical for every policy; see docs/phase5_design.md.
//
// NOT copyable or movable. The ID index stores list iterators into this book's own
// queues (and, for PoolPolicy, containers point at an arena owned by the book), so a
// copy would alias the original's nodes. (Earlier phases left the copy constructor
// implicitly enabled, which silently produced a corrupt book; deleting it is a fix.)
//
// Complexity (n = live orders, L = price levels on the touched side):
//   add     O(log L) + O(1) average for the ID index
//   cancel  O(log L) + O(1) average for the ID index and queue removal
//   best_*  O(1)
//   snapshot(depth) O(depth)
template <class Policy>
class BasicOrderBook {
public:
    using Level = BasicPriceLevel<Policy>;
    static constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();

    BasicOrderBook();
    ~BasicOrderBook() = default;
    BasicOrderBook(const BasicOrderBook&) = delete;
    BasicOrderBook& operator=(const BasicOrderBook&) = delete;
    BasicOrderBook(BasicOrderBook&&) = delete;
    BasicOrderBook& operator=(BasicOrderBook&&) = delete;

    // The checks performed before any mutation, in this order: quantity, time-in-force (a market
    // order cannot be GTC), price range (skipped for market orders), duplicate live ID.
    // Returns Ok if all pass. Valid for any order the engine accepts, including IOC/FOK/market.
    AddStatus precheck(const Order& order) const;

    // Inserts a RESTING order (GTC limit only) at the back of its price level's FIFO queue.
    // On any non-Ok status the book is left unchanged.
    AddStatus add(const Order& order);

    // Removes a live order. Returns false if the ID is not live (never added,
    // already cancelled). MatchingEngine turns this into an event.
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    // best_ask - best_bid; empty if either side is empty. Negative if crossed.
    std::optional<Price> spread() const;

    bool contains(OrderId id) const { return index_.contains(id); }
    std::optional<Order> find(OrderId id) const;

    // The highest-priority order on `side`: front of the queue at the best
    // price. Empty if that side is empty.
    std::optional<Order> best_order(Side side) const;
    // Fills `qty` of the best_order(side) (0 < qty <= its remaining quantity).
    // It keeps its queue position if partially filled and is removed (along
    // with its level, if now empty) when exhausted. Matching primitive.
    void execute_front(Side side, Quantity qty);
    // Quantity a taker could execute right now: the total resting on the opposite side at prices
    // acceptable to `limit` (best price first). Stops scanning once `needed` is reached, so the
    // result is "at least needed" or the exact total if that is smaller. O(levels scanned).
    Quantity fillable_quantity(Side taker_side, Price limit, Quantity needed) const;
    // Lowers a live order's quantity in place, keeping its queue position.
    // Precondition: the order is live and 0 < new_quantity <= its current quantity.
    void reduce_quantity(OrderId id, Quantity new_quantity);
    // True iff best bid >= best ask. Never true under MatchingEngine.
    bool is_crossed() const;

    std::size_t order_count() const noexcept { return index_.size(); }
    std::size_t level_count(Side side) const noexcept;
    // Aggregate resting quantity at a price (0 if no such level).
    Quantity quantity_at(Side side, Price price) const;
    // The FIFO queue at a price, or nullptr if there is no such level.
    const Level* level(Side side, Price price) const;

    // Top `depth` levels per side, best first. Deterministic.
    BookSnapshot snapshot(std::size_t depth = kAllLevels) const;

    // Checks every structural invariant (O(n)). On failure returns false and,
    // if `why` is non-null, describes the first violation found. add() and
    // cancel() assert this in builds without NDEBUG.
    bool validate(std::string* why = nullptr) const;

private:
    template <class T>
    using Alloc = typename Policy::template Allocator<T>;
    using BidLevels = std::map<Price, Level, std::greater<Price>, Alloc<std::pair<const Price, Level>>>;
    using AskLevels = std::map<Price, Level, std::less<Price>, Alloc<std::pair<const Price, Level>>>;

    // Where a live order sits: enough to cancel it without searching.
    struct Locator {
        Side side;
        Price price;
        typename Level::Handle handle;
    };
    using Index = std::unordered_map<OrderId, Locator, std::hash<OrderId>, std::equal_to<OrderId>,
                                     Alloc<std::pair<const OrderId, Locator>>>;

    template <class Levels>
    AddStatus add_to(Levels& levels, const Order& order);
    template <class Levels>
    void execute_front_of(Levels& levels, Quantity qty);
    template <class Levels>
    static Quantity fillable_in(const Levels& levels, Side taker_side, Price limit, Quantity needed);
    template <class Levels>
    static void reduce_in(Levels& levels, const Locator& loc, Quantity new_quantity);
    template <class Levels>
    static void erase_from(Levels& levels, const Locator& loc);
    template <class Levels>
    bool validate_side(const Levels& levels, Side side, std::size_t& seen, std::string* why) const;

    // Declared first so it is destroyed last: the containers below allocate from it.
    [[no_unique_address]] typename Policy::Resource resource_;
    BidLevels bids_;
    AskLevels asks_;
    Index index_;
};

using OrderBook = BasicOrderBook<StdPolicy>;
using PooledOrderBook = BasicOrderBook<PoolPolicy>;
extern template class BasicOrderBook<StdPolicy>;
extern template class BasicOrderBook<PoolPolicy>;

}  // namespace lob
