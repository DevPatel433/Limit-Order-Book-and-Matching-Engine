#include "workloads.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "lob/matching_engine.hpp"

namespace lob::bench {

namespace {

constexpr Price kMid = 1'000'000;

struct Rng {
    std::mt19937_64 gen;
    explicit Rng(std::uint64_t seed) : gen(seed) {}
    std::uint64_t below(std::uint64_t n) { return gen() % n; }          // [0, n)
    std::int64_t range(std::int64_t lo, std::int64_t hi) {              // [lo, hi]
        return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo + 1)));
    }
    bool chance(unsigned percent) { return below(100) < percent; }
};

// Builds an operation list while running the real engine (untimed) so it always
// knows which order IDs are live and where the touch is.
class Builder {
public:
    explicit Builder(std::uint64_t seed) : rng(seed) {}

    void add(Side side, Price price, Quantity qty) {
        const OrderId id = next_id_++;
        const Order o{id, side, price, qty};
        ops.push_back(BenchOp{false, o, 0});
        ev_.clear();
        eng_.submit(o, ev_);
        for (const Event& e : ev_)
            if (const auto* t = std::get_if<Trade>(&e))
                if (!eng_.book().contains(t->maker_id)) forget(t->maker_id);
        if (eng_.book().contains(id)) remember(id);
    }

    // Cancels a uniformly random live order. Returns false if none exist.
    bool cancel_random() {
        if (live_.empty()) return false;
        const OrderId id = live_[rng.below(live_.size())];
        ops.push_back(BenchOp{true, {}, id});
        ev_.clear();
        eng_.cancel(id, ev_);
        forget(id);
        return true;
    }

    Price best_bid_or(Price fallback) const { return eng_.book().best_bid().value_or(fallback); }
    Price best_ask_or(Price fallback) const { return eng_.book().best_ask().value_or(fallback); }
    std::size_t size() const { return ops.size(); }
    const std::vector<OrderId>& live_ids() const { return live_; }

    // Convenience: a resting-only order `level` ticks away from the mid.
    void add_resting(std::int64_t max_level) {
        const Side side = rng.below(2) ? Side::Buy : Side::Sell;
        const Price level = rng.range(1, max_level);
        add(side, side == Side::Buy ? kMid - level : kMid + level, 1 + rng.below(100));
    }

    Rng rng;
    std::vector<BenchOp> ops;

private:
    void remember(OrderId id) {
        pos_[id] = live_.size();
        live_.push_back(id);
    }
    void forget(OrderId id) {
        const auto it = pos_.find(id);
        if (it == pos_.end()) return;
        const std::size_t i = it->second;
        live_[i] = live_.back();
        pos_[live_[i]] = i;
        live_.pop_back();
        pos_.erase(id);
    }

    MatchingEngine eng_;
    std::vector<Event> ev_;
    std::vector<OrderId> live_;
    std::unordered_map<OrderId, std::size_t> pos_;
    OrderId next_id_{1};
};

std::size_t scaled(std::size_t base, double scale) {
    return static_cast<std::size_t>(static_cast<double>(base) * scale);
}

// ---- individual workloads ----------------------------------------------------

// Every order rests; nothing trades or cancels. Measures insertion into a book
// that keeps growing (100 levels per side, FIFO queues getting longer).
void build_non_crossing(Builder& b, std::size_t total) {
    while (b.size() < total) b.add_resting(100);
}

// Steady-state churn on a book of ~20k orders: half adds, half cancels of random live orders.
void build_cancel_heavy(Builder& b, std::size_t prefill, std::size_t total) {
    while (b.size() < prefill) b.add_resting(200);
    while (b.size() < total) {
        if (b.rng.chance(50) && b.cancel_random()) continue;
        b.add_resting(200);
    }
}

// Tight band around the mid: most orders cross and trade. Small cancel share.
void build_crossing_heavy(Builder& b, std::size_t prefill, std::size_t total) {
    while (b.size() < prefill) {
        const bool buy = b.rng.below(2);
        const Price d = b.rng.range(1, 10);
        b.add(buy ? Side::Buy : Side::Sell, buy ? kMid - d : kMid + d, 1 + b.rng.below(50));
    }
    while (b.size() < total) {
        if (b.rng.chance(5) && b.cancel_random()) continue;
        const Side side = b.rng.below(2) ? Side::Buy : Side::Sell;
        b.add(side, kMid + b.rng.range(-8, 8), 1 + b.rng.below(50));
    }
}

// A very wide, deep book (20,000 price levels per side, ~100k orders), then
// 50/50 add/cancel churn spread over all of it. Stresses ordered-map depth.
void build_many_levels(Builder& b, std::size_t prefill, std::size_t total) {
    while (b.size() < prefill) b.add_resting(20'000);
    while (b.size() < total) {
        if (b.rng.chance(50) && b.cancel_random()) continue;
        b.add_resting(20'000);
    }
}

// Cycles of 900 quiet operations (quotes added/cancelled near the touch) and
// 100 aggressive orders that each sweep several levels. Expect a heavy latency tail.
void build_bursty(Builder& b, std::size_t prefill, std::size_t total) {
    while (b.size() < prefill) b.add_resting(100);
    constexpr std::size_t kQuiet = 900, kBurst = 100;
    std::size_t phase_pos = 0;
    bool buy_next = true;
    while (b.size() < total) {
        if (phase_pos % (kQuiet + kBurst) < kQuiet) {
            if (b.rng.chance(30) && b.cancel_random()) {
                ++phase_pos;
                continue;
            }
            if (b.rng.below(2)) {
                const Price base = b.best_bid_or(kMid - 1);
                b.add(Side::Buy, std::max<Price>(1, base - b.rng.range(0, 20)), 1 + b.rng.below(100));
            } else {
                const Price base = b.best_ask_or(kMid + 1);
                b.add(Side::Sell, base + b.rng.range(0, 20), 1 + b.rng.below(100));
            }
        } else {
            const Quantity q = 100 + b.rng.below(201);
            if (buy_next) b.add(Side::Buy, b.best_ask_or(kMid + 1) + b.rng.range(0, 10), q);
            else b.add(Side::Sell, std::max<Price>(1, b.best_bid_or(kMid - 1) - b.rng.range(0, 10)), q);
            buy_next = !buy_next;
        }
        ++phase_pos;
    }
}

struct Spec {
    const char* name;
    const char* description;
    std::size_t prefill;
};

const Spec kSpecs[] = {
    {"non_crossing", "add-only, every order rests, 100 levels/side, book grows", 0},
    {"cancel_heavy", "50% adds / 50% cancels of random live orders, steady ~20k-order book", 20'000},
    {"crossing_heavy", "tight price band: most orders cross and trade, 5% cancels", 2'000},
    {"many_levels", "20,000 levels/side, ~100k resting orders, 50/50 add/cancel churn", 100'000},
    {"bursty", "900 quiet quote updates then 100 multi-level sweeps, repeating", 20'000},
};

}  // namespace

const std::vector<std::string>& workload_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const Spec& s : kSpecs) v.emplace_back(s.name);
        return v;
    }();
    return names;
}

Workload make_workload(const std::string& name, const WorkloadParams& p) {
    const Spec* spec = nullptr;
    for (const Spec& s : kSpecs)
        if (name == s.name) spec = &s;
    if (!spec) throw std::invalid_argument("unknown workload: " + name);

    Workload w;
    w.name = spec->name;
    w.description = spec->description;
    w.prefill = scaled(spec->prefill, p.prefill_scale);
    w.warmup = p.warmup;
    const std::size_t total = w.prefill + w.warmup + p.measured;

    Builder b(p.seed);
    b.ops.reserve(total);
    if (name == "non_crossing") build_non_crossing(b, total);
    else if (name == "cancel_heavy") build_cancel_heavy(b, w.prefill, total);
    else if (name == "crossing_heavy") build_crossing_heavy(b, w.prefill, total);
    else if (name == "many_levels") build_many_levels(b, w.prefill, total);
    else build_bursty(b, w.prefill, total);
    w.live_at_end = b.live_ids();
    w.ops = std::move(b.ops);
    return w;
}

std::vector<Command> to_commands(const Workload& w) {
    std::vector<Command> out;
    out.reserve(w.ops.size());
    for (const BenchOp& op : w.ops) {
        if (op.is_cancel) out.emplace_back(CancelCommand{op.cancel_id});
        else out.emplace_back(NewCommand{op.order});
    }
    return out;
}

}  // namespace lob::bench
