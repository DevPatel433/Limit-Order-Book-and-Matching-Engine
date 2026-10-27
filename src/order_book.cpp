#include "lob/order_book.hpp"

#include <cassert>
#include <sstream>

namespace lob {

const char* to_string(AddStatus s) noexcept {
    switch (s) {
        case AddStatus::Ok: return "Ok";
        case AddStatus::DuplicateId: return "DuplicateId";
        case AddStatus::InvalidPrice: return "InvalidPrice";
        case AddStatus::InvalidQuantity: return "InvalidQuantity";
        case AddStatus::QuantityOverflow: return "QuantityOverflow";
        case AddStatus::InvalidTimeInForce: return "InvalidTimeInForce";
    }
    return "Unknown";
}

namespace {

template <class Levels>
std::vector<LevelSnapshot> take_levels(const Levels& levels, std::size_t depth) {
    std::vector<LevelSnapshot> out;
    out.reserve(depth < levels.size() ? depth : levels.size());
    for (const auto& [price, lvl] : levels) {
        if (out.size() == depth) break;
        out.push_back({price, lvl.total_quantity(), lvl.order_count()});
    }
    return out;
}

template <class Levels>
const typename Levels::mapped_type* find_level(const Levels& levels, Price price) {
    auto it = levels.find(price);
    return it == levels.end() ? nullptr : &it->second;
}

}  // namespace

template <class P>
BasicOrderBook<P>::BasicOrderBook()
    : resource_(),
      bids_(std::greater<Price>{}, P::template make_allocator<typename BidLevels::value_type>(resource_)),
      asks_(std::less<Price>{}, P::template make_allocator<typename AskLevels::value_type>(resource_)),
      index_(0, std::hash<OrderId>{}, std::equal_to<OrderId>{},
             P::template make_allocator<typename Index::value_type>(resource_)) {}

// ---------------------------------------------------------------- mutation --

template <class P>
template <class Levels>
AddStatus BasicOrderBook<P>::add_to(Levels& levels, const Order& order) {
    auto it = levels.find(order.price);
    if (it == levels.end()) {
        it = levels.try_emplace(order.price, order.price, P::template make_allocator<Order>(resource_)).first;
    } else if (!it->second.can_accept(order.quantity)) {
        return AddStatus::QuantityOverflow;  // book untouched
    }
    const auto handle = it->second.append(order);
    index_.emplace(order.id, Locator{order.side, order.price, handle});
    return AddStatus::Ok;
}

template <class P>
AddStatus BasicOrderBook<P>::precheck(const Order& order) const {
    if (order.quantity == 0) return AddStatus::InvalidQuantity;
    if (order.type == OrderType::Market) {
        if (order.tif == TimeInForce::GTC) return AddStatus::InvalidTimeInForce;  // could never rest
    } else if (order.price < kMinPrice || order.price > kMaxPrice) {
        return AddStatus::InvalidPrice;
    }
    if (index_.contains(order.id)) return AddStatus::DuplicateId;
    return AddStatus::Ok;
}

template <class P>
AddStatus BasicOrderBook<P>::add(const Order& order) {
    // Validate everything before touching state so failures are no-ops.
    if (const AddStatus pre = precheck(order); pre != AddStatus::Ok) return pre;
    // Only GTC limit orders can rest; IOC/FOK/market orders are the engine's business.
    if (order.type != OrderType::Limit || order.tif != TimeInForce::GTC) return AddStatus::InvalidTimeInForce;

    const AddStatus status = order.side == Side::Buy ? add_to(bids_, order)
                                                      : add_to(asks_, order);
#ifndef NDEBUG
    std::string why;
    assert(validate(&why) && "OrderBook invariant violated after add");
#endif
    return status;
}

template <class P>
template <class Levels>
void BasicOrderBook<P>::erase_from(Levels& levels, const Locator& loc) {
    auto it = levels.find(loc.price);
    assert(it != levels.end());
    it->second.erase(loc.handle);
    if (it->second.empty()) levels.erase(it);  // never leave empty levels behind
}

template <class P>
bool BasicOrderBook<P>::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) return false;

    const Locator loc = found->second;
    index_.erase(found);
    if (loc.side == Side::Buy) erase_from(bids_, loc);
    else erase_from(asks_, loc);
#ifndef NDEBUG
    std::string why;
    assert(validate(&why) && "OrderBook invariant violated after cancel");
#endif
    return true;
}

// ----------------------------------------------------------------- queries --

template <class P>
std::optional<Price> BasicOrderBook<P>::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

template <class P>
std::optional<Price> BasicOrderBook<P>::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

template <class P>
std::optional<Price> BasicOrderBook<P>::spread() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;  // no overflow: see kMaxPrice
}

template <class P>
std::optional<Order> BasicOrderBook<P>::find(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return *it->second.handle;
}

template <class P>
std::optional<Order> BasicOrderBook<P>::best_order(Side side) const {
    if (side == Side::Buy) {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->second.orders().front();
    }
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->second.orders().front();
}

template <class P>
bool BasicOrderBook<P>::is_crossed() const {
    return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
}

template <class P>
template <class Levels>
void BasicOrderBook<P>::execute_front_of(Levels& levels, Quantity qty) {
    assert(!levels.empty());
    const auto it = levels.begin();
    const OrderId id = it->second.orders().front().id;
    if (it->second.reduce_front(qty)) index_.erase(id);  // handles of other orders stay valid
    if (it->second.empty()) levels.erase(it);
}

template <class P>
template <class Levels>
Quantity BasicOrderBook<P>::fillable_in(const Levels& levels, Side taker_side, Price limit, Quantity needed) {
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    Quantity total = 0;
    for (const auto& [price, lvl] : levels) {  // best price first
        const bool acceptable = taker_side == Side::Buy ? price <= limit : price >= limit;
        if (!acceptable) break;
        total = lvl.total_quantity() > kMax - total ? kMax : total + lvl.total_quantity();
        if (total >= needed) break;
    }
    return total;
}

template <class P>
Quantity BasicOrderBook<P>::fillable_quantity(Side taker_side, Price limit, Quantity needed) const {
    // A buyer consumes asks; a seller consumes bids.
    return taker_side == Side::Buy ? fillable_in(asks_, taker_side, limit, needed)
                                   : fillable_in(bids_, taker_side, limit, needed);
}

template <class P>
template <class Levels>
void BasicOrderBook<P>::reduce_in(Levels& levels, const Locator& loc, Quantity new_quantity) {
    const auto it = levels.find(loc.price);
    assert(it != levels.end());
    it->second.reduce(loc.handle, new_quantity);
}

template <class P>
void BasicOrderBook<P>::reduce_quantity(OrderId id, Quantity new_quantity) {
    const auto found = index_.find(id);
    assert(found != index_.end());
    const Locator& loc = found->second;
    if (loc.side == Side::Buy) reduce_in(bids_, loc, new_quantity);
    else reduce_in(asks_, loc, new_quantity);
#ifndef NDEBUG
    std::string why;
    assert(validate(&why) && "OrderBook invariant violated after reduce_quantity");
#endif
}

template <class P>
void BasicOrderBook<P>::execute_front(Side side, Quantity qty) {
    if (side == Side::Buy) execute_front_of(bids_, qty);
    else execute_front_of(asks_, qty);
#ifndef NDEBUG
    std::string why;
    assert(validate(&why) && "OrderBook invariant violated after execute_front");
#endif
}

template <class P>
std::size_t BasicOrderBook<P>::level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

template <class P>
const typename BasicOrderBook<P>::Level* BasicOrderBook<P>::level(Side side, Price price) const {
    return side == Side::Buy ? find_level(bids_, price) : find_level(asks_, price);
}

template <class P>
Quantity BasicOrderBook<P>::quantity_at(Side side, Price price) const {
    const Level* lvl = level(side, price);
    return lvl ? lvl->total_quantity() : 0;
}

template <class P>
BookSnapshot BasicOrderBook<P>::snapshot(std::size_t depth) const {
    return BookSnapshot{take_levels(bids_, depth), take_levels(asks_, depth)};
}

// -------------------------------------------------------------- invariants --

template <class P>
template <class Levels>
bool BasicOrderBook<P>::validate_side(const Levels& levels, Side side, std::size_t& seen,
                              std::string* why) const {
    auto fail = [&](const std::string& msg) {
        if (why) *why = msg;
        return false;
    };
    for (const auto& [price, lvl] : levels) {
        if (lvl.empty()) return fail("empty price level left in book at " + std::to_string(price));
        if (lvl.price() != price) return fail("level key/price mismatch at " + std::to_string(price));
        if (price < kMinPrice || price > kMaxPrice) return fail("level price out of range");

        Quantity sum = 0;
        std::size_t count = 0;
        for (auto it = lvl.orders().begin(); it != lvl.orders().end(); ++it) {
            const Order& o = *it;
            if (o.quantity == 0) return fail("zero-quantity live order " + std::to_string(o.id));
            if (o.side != side) return fail("order on wrong side " + std::to_string(o.id));
            if (o.price != price) return fail("order in wrong level " + std::to_string(o.id));

            const auto idx = index_.find(o.id);
            if (idx == index_.end()) return fail("live order missing from index " + std::to_string(o.id));
            if (std::addressof(*idx->second.handle) != std::addressof(o))
                return fail("index handle does not point at order " + std::to_string(o.id));
            if (idx->second.side != side || idx->second.price != price)
                return fail("index locator stale for order " + std::to_string(o.id));

            sum += o.quantity;  // cannot overflow: can_accept() guarded every append
            ++count;
        }
        if (sum != lvl.total_quantity()) return fail("level total quantity mismatch at " + std::to_string(price));
        if (count != lvl.order_count()) return fail("level order count mismatch at " + std::to_string(price));
        seen += count;
    }
    return true;
}

template <class P>
bool BasicOrderBook<P>::validate(std::string* why) const {
    std::size_t seen = 0;
    if (!validate_side(bids_, Side::Buy, seen, why)) return false;
    if (!validate_side(asks_, Side::Sell, seen, why)) return false;
    // Every order seen was found in the index, so equal counts mean the index
    // holds no extra (dangling) entries and no order appears in two levels.
    if (seen != index_.size()) {
        if (why) *why = "index size " + std::to_string(index_.size()) +
                        " != live orders in levels " + std::to_string(seen);
        return false;
    }
    return true;
}

template class BasicOrderBook<StdPolicy>;
template class BasicOrderBook<PoolPolicy>;

}  // namespace lob
