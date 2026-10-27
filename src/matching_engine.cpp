#include "lob/matching_engine.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace lob {

namespace {

// Does a taker on `taker_side` with limit `limit` trade against a resting order at `resting`?
constexpr bool crosses(Side taker_side, Price limit, Price resting) noexcept {
    return taker_side == Side::Buy ? resting <= limit : resting >= limit;
}

}  // namespace

template <class P>
void BasicMatchingEngine<P>::match_and_dispose(const Order& taker, std::vector<Event>& out) {
    const Price limit = effective_limit(taker);
    const Side maker_side = opposite(taker.side);

    // Sweep the opposing side best price first, FIFO within a price.
    Quantity remaining = taker.quantity;
    while (remaining > 0) {
        const auto maker = book_.best_order(maker_side);
        if (!maker || !crosses(taker.side, limit, maker->price)) break;

        const Quantity qty = std::min(remaining, maker->quantity);  // never over-executes
        book_.execute_front(maker_side, qty);
        out.emplace_back(Trade{maker->id, taker.id, taker.side, maker->price, qty});
        remaining -= qty;
    }
    if (remaining == 0) return;

    if (taker.tif != TimeInForce::GTC) {
        out.emplace_back(Expired{taker.id, remaining});  // IOC / market / (FOK only if pre-check let it through: never)
        return;
    }

    // GTC remainder rests. This cannot fail: the taker crossed, so every resting order on its own
    // side is strictly better-priced for the counterparty than its limit, hence no level exists at its
    // price yet (so no overflow), and its ID is not live (it passed the duplicate check, or was
    // removed by an amend before this call).
    Order rest = taker;
    rest.quantity = remaining;
    [[maybe_unused]] const AddStatus st = book_.add(rest);
    assert(st == AddStatus::Ok);
}

template <class P>
void BasicMatchingEngine<P>::submit(const Order& order, std::vector<Event>& out) {
    // 1. Reject malformed orders and duplicate live IDs before anything else.
    if (const AddStatus pre = book_.precheck(order); pre != AddStatus::Ok) {
        out.emplace_back(Rejected{order.id, to_reject_reason(pre)});
        return;
    }

    const Accepted accepted{order.id, order.side, order.price, order.quantity, order.tif, order.type};
    const Price limit = effective_limit(order);
    const auto best_opposing = book_.best_order(opposite(order.side));
    const bool crosses_now = best_opposing && crosses(order.side, limit, best_opposing->price);

    // 2. Nothing to execute against.
    if (!crosses_now) {
        if (order.tif == TimeInForce::GTC) {
            // Rest the whole order. This is the only path where the book can still refuse (level
            // quantity overflow), and nothing has been traded yet, so a rejection is clean.
            if (const AddStatus st = book_.add(order); st != AddStatus::Ok) {
                out.emplace_back(Rejected{order.id, to_reject_reason(st)});
                return;
            }
            out.emplace_back(accepted);
            assert(!book_.is_crossed());
        } else {
            // IOC / FOK / market with no executable liquidity: accepted, then nothing happens.
            out.emplace_back(accepted);
            out.emplace_back(Expired{order.id, order.quantity});
        }
        return;
    }

    // 3. Crossing.
    out.emplace_back(accepted);
    if (order.tif == TimeInForce::FOK &&
        book_.fillable_quantity(order.side, limit, order.quantity) < order.quantity) {
        out.emplace_back(Expired{order.id, order.quantity});  // all or nothing: nothing traded
        return;
    }
    match_and_dispose(order, out);
    assert(!book_.is_crossed());
}

template <class P>
void BasicMatchingEngine<P>::cancel(OrderId id, std::vector<Event>& out) {
    const auto live = book_.find(id);
    if (!live) {
        out.emplace_back(Rejected{id, RejectReason::UnknownOrder});
        return;
    }
    [[maybe_unused]] const bool removed = book_.cancel(id);
    assert(removed);
    out.emplace_back(Cancelled{id, live->quantity});
}

template <class P>
void BasicMatchingEngine<P>::amend(OrderId id, Price new_price, Quantity new_quantity, std::vector<Event>& out) {
    const auto cur = book_.find(id);
    if (!cur) {
        out.emplace_back(Rejected{id, RejectReason::UnknownOrder});
        return;
    }
    if (new_quantity == 0) {  // use cancel() to remove an order
        out.emplace_back(Rejected{id, RejectReason::InvalidQuantity});
        return;
    }
    if (new_price < kMinPrice || new_price > kMaxPrice) {
        out.emplace_back(Rejected{id, RejectReason::InvalidPrice});
        return;
    }

    // Same price and not larger: reduce in place, queue position untouched.
    if (new_price == cur->price && new_quantity <= cur->quantity) {
        book_.reduce_quantity(id, new_quantity);
        out.emplace_back(Amended{id, cur->price, cur->quantity, new_price, new_quantity, true});
        return;
    }

    // Otherwise time priority is lost: remove and re-enter as a fresh GTC order with the same ID.
    // Validate first so a failure cannot lose the original order. If the new price does not cross,
    // the replacement rests in a level whose total must not overflow (the old order leaves its own
    // level first). If it crosses, it only ever rests in a fresh level, which cannot overflow.
    const auto best_opposing = book_.best_order(opposite(cur->side));
    const bool crosses_now = best_opposing && crosses(cur->side, new_price, best_opposing->price);
    if (!crosses_now) {
        Quantity existing = book_.quantity_at(cur->side, new_price);
        if (new_price == cur->price) existing -= cur->quantity;
        if (new_quantity > std::numeric_limits<Quantity>::max() - existing) {
            out.emplace_back(Rejected{id, RejectReason::QuantityOverflow});
            return;
        }
    }

    [[maybe_unused]] const bool removed = book_.cancel(id);
    assert(removed);
    out.emplace_back(Amended{id, cur->price, cur->quantity, new_price, new_quantity, false});
    match_and_dispose(Order(id, cur->side, new_price, new_quantity), out);
    assert(!book_.is_crossed());
}

template <class P>
std::vector<Event> BasicMatchingEngine<P>::submit(const Order& order) {
    std::vector<Event> out;
    submit(order, out);
    return out;
}

template <class P>
std::vector<Event> BasicMatchingEngine<P>::cancel(OrderId id) {
    std::vector<Event> out;
    cancel(id, out);
    return out;
}

template <class P>
std::vector<Event> BasicMatchingEngine<P>::amend(OrderId id, Price new_price, Quantity new_quantity) {
    std::vector<Event> out;
    amend(id, new_price, new_quantity, out);
    return out;
}

template class BasicMatchingEngine<StdPolicy>;
template class BasicMatchingEngine<PoolPolicy>;

}  // namespace lob
