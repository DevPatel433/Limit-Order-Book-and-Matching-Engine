#include "lob/replay.hpp"

#include <limits>
#include <ostream>
#include <sstream>

#include "lob/apply.hpp"
#include "lob/checker.hpp"
#include "lob/matching_engine.hpp"

namespace lob {

namespace {

Quantity sat_add(Quantity a, Quantity b) {
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    return b > kMax - a ? kMax : a + b;
}

}  // namespace

void tally_event(const Event& e, ReplayStats& st) {
    if (const auto* a = std::get_if<Accepted>(&e)) {
        ++st.accepted;
        st.accepted_quantity = sat_add(st.accepted_quantity, a->quantity);
    } else if (std::holds_alternative<Rejected>(e)) {
        ++st.rejected;
    } else if (const auto* t = std::get_if<Trade>(&e)) {
        ++st.trades;
        st.traded_quantity = sat_add(st.traded_quantity, t->quantity);
    } else if (const auto* c = std::get_if<Cancelled>(&e)) {
        ++st.cancelled;
        st.cancelled_quantity = sat_add(st.cancelled_quantity, c->remaining);
    } else if (const auto* x = std::get_if<Expired>(&e)) {
        ++st.expired;
        st.expired_quantity = sat_add(st.expired_quantity, x->remaining);
    } else if (const auto* m = std::get_if<Amended>(&e)) {
        ++st.amended;
        if (m->new_quantity >= m->old_quantity) st.amend_added_quantity = sat_add(st.amend_added_quantity, m->new_quantity - m->old_quantity);
        else st.amend_removed_quantity = sat_add(st.amend_removed_quantity, m->old_quantity - m->new_quantity);
    }
}

void merge_stats(ReplayStats& into, const ReplayStats& from) {
    into.commands += from.commands;
    into.accepted += from.accepted;
    into.rejected += from.rejected;
    into.trades += from.trades;
    into.cancelled += from.cancelled;
    into.expired += from.expired;
    into.amended += from.amended;
    into.accepted_quantity = sat_add(into.accepted_quantity, from.accepted_quantity);
    into.traded_quantity = sat_add(into.traded_quantity, from.traded_quantity);
    into.cancelled_quantity = sat_add(into.cancelled_quantity, from.cancelled_quantity);
    into.expired_quantity = sat_add(into.expired_quantity, from.expired_quantity);
    into.amend_added_quantity = sat_add(into.amend_added_quantity, from.amend_added_quantity);
    into.amend_removed_quantity = sat_add(into.amend_removed_quantity, from.amend_removed_quantity);
}

namespace {
void tally(const std::vector<Event>& events, ReplayStats& st) {
    for (const Event& e : events) tally_event(e, st);
}

}  // namespace

std::string format_summary(const ReplayResult& r) {
    const ReplayStats& s = r.stats;
    std::ostringstream os;
    os << "SUMMARY commands=" << s.commands << " accepted=" << s.accepted << " rejected=" << s.rejected
       << " trades=" << s.trades << " cancelled=" << s.cancelled << " traded_qty=" << s.traded_quantity
       << " resting=" << r.resting_orders;
    // Only shown when used, so replay outputs that predate these features are unchanged.
    if (s.expired) os << " expired=" << s.expired;
    if (s.amended) os << " amended=" << s.amended;
    return os.str();
}

std::string format_snapshot(const BookSnapshot& s, std::size_t depth) {
    std::ostringstream os;
    os << "BOOK depth=";
    if (depth == OrderBook::kAllLevels) os << "all";
    else os << depth;
    os << '\n';

    const std::size_t asks = std::min(depth, s.asks.size());
    const std::size_t bids = std::min(depth, s.bids.size());
    for (std::size_t i = asks; i-- > 0;)
        os << "  ASK " << s.asks[i].price << " x " << s.asks[i].quantity << " (" << s.asks[i].order_count << ")\n";
    os << "  ----\n";
    for (std::size_t i = 0; i < bids; ++i)
        os << "  BID " << s.bids[i].price << " x " << s.bids[i].quantity << " (" << s.bids[i].order_count << ")\n";
    os << "END\n";
    return os.str();
}

namespace {
template <class Engine>
ReplayResult replay_with(const std::vector<Command>& commands, std::ostream& out, const ReplayOptions& opt) {
    Engine engine;
    ConsistencyChecker checker;
    ReplayResult result;
    std::vector<Event> events;

    if (opt.csv) out << csv_header() << '\n';

    std::size_t index = 0, seq = 0;
    for (const Command& cmd : commands) {
        ++index;
        ++result.stats.commands;
        events.clear();

        if (const auto* snap = std::get_if<SnapshotCommand>(&cmd)) {
            if (!opt.csv) {
                const std::size_t depth = snap->depth.value_or(opt.default_depth);
                out << format_snapshot(engine.book().snapshot(depth), depth);
            }
            continue;
        }
        apply_command(engine, cmd, events);

        tally(events, result.stats);
        if (opt.csv) {
            for (const Event& e : events) out << to_csv_row(++seq, index, e) << '\n';
        } else if (opt.print_events) {
            for (const Event& e : events) out << to_string(e) << '\n';
        }

        if (opt.check) {
            if (auto v = checker.observe(events, engine)) {
                result.violation = Violation{index, std::move(*v)};
                break;
            }
        }
    }

    result.final_book = engine.book().snapshot();
    result.resting_orders = engine.book().order_count();

    if (opt.csv) return result;  // pure CSV: no snapshot, no SUMMARY (the CLI reports it on stderr)
    if (opt.final_snapshot) out << format_snapshot(engine.book().snapshot(opt.default_depth), opt.default_depth);
    out << format_summary(result) << '\n';
    return result;
}
}  // namespace

ReplayResult replay(const std::vector<Command>& commands, std::ostream& out, const ReplayOptions& opt) {
    return opt.engine == EngineKind::Pooled ? replay_with<PooledMatchingEngine>(commands, out, opt)
                                            : replay_with<MatchingEngine>(commands, out, opt);
}

}  // namespace lob
