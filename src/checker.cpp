#include "lob/checker.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace lob {

namespace {

using Violation = std::optional<std::string>;

Violation err(std::string msg) { return Violation{std::move(msg)}; }

std::string id_str(OrderId id) { return std::to_string(id); }

// Is price `a` strictly better than `b` for a resting order on `maker_side`?
bool strictly_better(Side maker_side, Price a, Price b) {
    return maker_side == Side::Sell ? a < b : a > b;
}

// Deliberately NOT effective_limit() from order.hpp: the checker must not share that logic with the
// engine, or a bug in it would be invisible here (a mutation test found exactly that).
Price market_limit(Side side) { return side == Side::Buy ? kMaxPrice : kMinPrice; }

bool crosses(Side taker_side, Price limit, Price resting) {
    return taker_side == Side::Buy ? resting <= limit : resting >= limit;
}

}  // namespace

template <class Policy>
std::optional<std::string> ConsistencyChecker::observe(const std::vector<Event>& events,
                                                       const BasicMatchingEngine<Policy>& engine) {
    if (events.empty()) return err("command produced no events");

    // The "taker" is whatever is currently executing against the book: a new order (Accepted) or a
    // re-priced amended order (Amended that lost priority).
    struct Taker {
        OrderId id;
        Side side;
        Price limit;  // effective limit (extreme for market orders)
        TimeInForce tif;
        Quantity quantity;   // quantity at entry
        Quantity remaining;
        Quantity traded;
        bool expired;
    };
    std::optional<Taker> taker;

    auto crossing_liquidity = [&](const Taker& t) {  // opposite-side ledger quantity the taker could still execute against
        Quantity total = 0;
        for (const auto& [id, l] : live_)
            if (l.order.side != t.side && crosses(t.side, t.limit, l.order.price)) {
                total = l.order.quantity > std::numeric_limits<Quantity>::max() - total
                            ? std::numeric_limits<Quantity>::max()
                            : total + l.order.quantity;
            }
        return total;
    };

    for (const Event& ev : events) {
        if (taker && taker->expired) return err("event after Expired (Expired must be last for an order)");

        if (const auto* a = std::get_if<Accepted>(&ev)) {
            if (taker) return err("Accepted while a previous taker is still open");
            if (live_.contains(a->id)) return err("Accepted id=" + id_str(a->id) + " which is already live");
            if (a->quantity == 0) return err("Accepted with zero quantity");
            if (a->type == OrderType::Market && a->tif == TimeInForce::GTC) return err("Accepted a GTC market order");
            const Price limit = a->type == OrderType::Market ? market_limit(a->side) : a->price;
            taker = Taker{a->id, a->side, limit, a->tif, a->quantity, a->quantity, 0, false};

        } else if (const auto* t = std::get_if<Trade>(&ev)) {
            if (!taker) return err("Trade without a preceding Accepted or Amended");
            if (t->taker_id != taker->id || t->taker_side != taker->side)
                return err("Trade taker does not match the open order");
            const auto it = live_.find(t->maker_id);
            if (it == live_.end()) return err("Trade against non-live maker " + id_str(t->maker_id));
            Live& maker = it->second;
            const Side maker_side = opposite(taker->side);
            if (maker.order.side != maker_side) return err("Trade against same-side maker " + id_str(t->maker_id));
            if (t->price != maker.order.price) return err("Trade price is not the maker's price");
            if (!crosses(taker->side, taker->limit, t->price)) return err("Trade price beyond taker's limit");
            if (t->quantity == 0) return err("Trade with zero quantity");
            if (t->quantity > maker.order.quantity) return err("Trade exceeds maker's remaining quantity");
            if (t->quantity > taker->remaining) return err("Trade exceeds taker's remaining quantity");

            // Price-time priority: nothing on the maker side may be better than this maker.
            for (const auto& [id, other] : live_) {
                if (other.order.side != maker_side || id == t->maker_id) continue;
                if (strictly_better(maker_side, other.order.price, maker.order.price))
                    return err("price priority violated: order " + id_str(id) + " is better than " + id_str(t->maker_id));
                if (other.order.price == maker.order.price && other.seq < maker.seq)
                    return err("time priority violated: order " + id_str(id) + " arrived before " + id_str(t->maker_id));
            }

            maker.order.quantity -= t->quantity;
            taker->remaining -= t->quantity;
            taker->traded += t->quantity;
            if (maker.order.quantity == 0) live_.erase(it);

        } else if (const auto* r = std::get_if<Rejected>(&ev)) {
            if (taker) return err("Rejected after Accepted");
            if (r->reason == RejectReason::DuplicateId && !live_.contains(r->id))
                return err("DuplicateId rejection for id=" + id_str(r->id) + " which is not live");
            if (r->reason == RejectReason::UnknownOrder && live_.contains(r->id))
                return err("UnknownOrder rejection for id=" + id_str(r->id) + " which is live");

        } else if (const auto* c = std::get_if<Cancelled>(&ev)) {
            if (taker) return err("Cancelled after Accepted");
            const auto it = live_.find(c->id);
            if (it == live_.end()) return err("Cancelled non-live id=" + id_str(c->id));
            if (it->second.order.quantity != c->remaining) return err("Cancelled reports wrong remaining quantity");
            live_.erase(it);

        } else if (const auto* x = std::get_if<Expired>(&ev)) {
            if (!taker) return err("Expired without an open order");
            if (x->id != taker->id) return err("Expired id does not match the open order");
            if (taker->tif == TimeInForce::GTC) return err("Expired for a GTC order (it should rest)");
            if (x->remaining == 0 || x->remaining != taker->remaining) return err("Expired reports wrong remaining quantity");
            taker->expired = true;

        } else {
            const auto& m = std::get<Amended>(ev);
            if (taker) return err("Amended while another order is executing");
            const auto it = live_.find(m.id);
            if (it == live_.end()) return err("Amended non-live id=" + id_str(m.id));
            const Order cur = it->second.order;
            if (m.old_price != cur.price || m.old_quantity != cur.quantity)
                return err("Amended old values differ from the live order " + id_str(m.id));
            if (m.new_quantity == 0) return err("Amended to zero quantity");
            if (m.new_price < kMinPrice || m.new_price > kMaxPrice) return err("Amended to an out-of-range price");
            const bool should_keep = m.new_price == cur.price && m.new_quantity <= cur.quantity;
            if (m.keeps_priority != should_keep)
                return err(std::string("Amended claims priority ") + (m.keeps_priority ? "kept" : "lost") +
                           " but the rules say " + (should_keep ? "kept" : "lost"));
            if (should_keep) {
                it->second.order.quantity = m.new_quantity;  // in place: same sequence number
            } else {
                live_.erase(it);  // re-enters as a taker; may trade, then rests at the back of its level
                taker = Taker{m.id, cur.side, m.new_price, TimeInForce::GTC, m.new_quantity, m.new_quantity, 0, false};
            }
        }
    }

    // ---- End of this command's events: what happened to the open order? ---------------------
    if (taker && taker->remaining > 0) {
        if (taker->expired) {
            if (taker->tif == TimeInForce::FOK) {
                if (taker->traded != 0) return err("FOK order was partially filled");
                if (crossing_liquidity(*taker) >= taker->quantity)
                    return err("FOK order expired although it was fully fillable");
            } else if (crossing_liquidity(*taker) != 0) {
                return err("IOC/market order expired while crossing liquidity remained");
            }
        } else {
            if (taker->tif != TimeInForce::GTC) return err("IOC/FOK remainder was neither filled nor expired");
            if (crossing_liquidity(*taker) != 0) return err("GTC remainder rests while crossing liquidity remains");
            // A GTC remainder rests at the back of its level.
            live_[taker->id] = Live{Order(taker->id, taker->side, taker->limit, taker->remaining), next_seq_++};
        }
    }

    // ---- Compare the engine's book to the ledger -------------------------------
    const auto& book = engine.book();
    std::string why;
    if (!book.validate(&why)) return err("book invariant violated: " + why);
    if (book.is_crossed()) return err("book is crossed after command");
    if (book.order_count() != live_.size())
        return err("book has " + std::to_string(book.order_count()) + " orders, ledger has " +
                   std::to_string(live_.size()));

    const BookSnapshot snap = book.snapshot();
    for (std::size_t i = 1; i < snap.bids.size(); ++i)
        if (snap.bids[i - 1].price <= snap.bids[i].price) return err("bid levels not strictly descending");
    for (std::size_t i = 1; i < snap.asks.size(); ++i)
        if (snap.asks[i - 1].price >= snap.asks[i].price) return err("ask levels not strictly ascending");

    std::map<std::pair<int, Price>, std::vector<const Live*>> ledger_levels;
    for (const auto& [id, l] : live_)
        ledger_levels[{static_cast<int>(l.order.side), l.order.price}].push_back(&l);

    std::size_t bid_levels = 0, ask_levels = 0;
    for (auto& [key, queue] : ledger_levels) {
        const Side side = static_cast<Side>(key.first);
        (side == Side::Buy ? bid_levels : ask_levels)++;
        std::sort(queue.begin(), queue.end(), [](const Live* a, const Live* b) { return a->seq < b->seq; });
        const auto* lvl = book.level(side, key.second);
        if (!lvl) return err("ledger level " + std::to_string(key.second) + " missing from book");
        if (lvl->order_count() != queue.size()) return err("level order count differs from ledger at " + std::to_string(key.second));
        std::size_t i = 0;
        for (const Order& o : lvl->orders()) {
            if (!(o == queue[i]->order))
                return err("order " + id_str(o.id) + " (or its queue position) differs from ledger at price " +
                           std::to_string(key.second));
            ++i;
        }
    }
    if (book.level_count(Side::Buy) != bid_levels || book.level_count(Side::Sell) != ask_levels)
        return err("book has levels the ledger does not");

    return std::nullopt;
}

template std::optional<std::string> ConsistencyChecker::observe<StdPolicy>(
    const std::vector<Event>&, const BasicMatchingEngine<StdPolicy>&);
template std::optional<std::string> ConsistencyChecker::observe<PoolPolicy>(
    const std::vector<Event>&, const BasicMatchingEngine<PoolPolicy>&);

}  // namespace lob
