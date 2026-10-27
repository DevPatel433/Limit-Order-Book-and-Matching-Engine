#include "lob/multi_replay.hpp"

#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>

#include "lob/checker.hpp"
#include "lob/pipeline.hpp"

namespace lob {

namespace {

std::string symbol_line(const SymbolTable& t, SymbolId s, const Event& e) {
    return "@" + t.name(s) + " " + to_string(e);
}

// Prints one event in the selected format and advances the event counter.
void emit_event(std::ostream& out, const SymbolTable& t, const MultiReplayOptions& opt, std::size_t& event_seq,
                std::uint64_t cmd_index, SymbolId s, const Event& e) {
    ++event_seq;
    if (opt.csv) out << t.name(s) << ',' << to_csv_row(event_seq, static_cast<std::size_t>(cmd_index), e) << '\n';
    else if (opt.print_events) out << symbol_line(t, s, e) << '\n';
}

template <class Policy>
MultiReplayResult sequential_replay(const SymbolTable& t, const std::vector<SymbolCommand>& commands,
                                    std::ostream& out, const MultiReplayOptions& opt,
                                    std::vector<ReplayStats>& per) {
    BasicMultiEngine<Policy> engines;
    std::vector<std::unique_ptr<ConsistencyChecker>> checkers(t.size());
    std::vector<Event> events;
    MultiReplayResult result;

    std::size_t index = 0, event_seq = 0;
    for (const SymbolCommand& sc : commands) {
        ++index;
        ++per[sc.symbol].commands;
        events.clear();

        auto& engine = engines.engine(sc.symbol);  // created on first use, like the pipeline does
        if (opt.check && !checkers[sc.symbol]) checkers[sc.symbol] = std::make_unique<ConsistencyChecker>();  // FRESH engine

        if (const auto* snap = std::get_if<SnapshotCommand>(&sc.command)) {
            if (!opt.csv) {
                const std::size_t depth = snap->depth.value_or(opt.default_depth);
                out << '@' << t.name(sc.symbol) << ' ' << format_snapshot(engine.book().snapshot(depth), depth);
            }
            continue;
        }
        if (opt.journal) opt.journal(sc);  // write-ahead: logged before it can change any book
        apply_command(engine, sc.command, events);

        for (const Event& e : events) {
            tally_event(e, per[sc.symbol]);
            emit_event(out, t, opt, event_seq, index, sc.symbol, e);
        }
        if (opt.check) {
            if (auto v = checkers[sc.symbol]->observe(events, engine)) {
                result.violation = Violation{index, "[" + t.name(sc.symbol) + "] " + std::move(*v)};
                break;
            }
        }
    }

    result.symbols.resize(t.size());
    engines.for_each([&](SymbolId s, const typename BasicMultiEngine<Policy>::Engine& e) {
        result.symbols[s].final_book = e.book().snapshot();
        result.symbols[s].resting = e.book().order_count();
    });
    return result;
}

// First difference between two event streams, as text; nullopt if identical.
std::optional<std::string> diff_events(const SymbolTable& t, const std::vector<SymbolEvent>& a,
                                       const std::vector<SymbolEvent>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (!(a[i] == b[i])) {
            std::ostringstream os;
            os << "event " << (i + 1) << " differs: threaded cmd " << a[i].seq << " '" << symbol_line(t, a[i].symbol, a[i].event)
               << "' vs sequential cmd " << b[i].seq << " '" << symbol_line(t, b[i].symbol, b[i].event) << "'";
            return os.str();
        }
    }
    if (a.size() != b.size())
        return "event count differs: threaded " + std::to_string(a.size()) + " vs sequential " + std::to_string(b.size());
    return std::nullopt;
}

}  // namespace

MultiReplayResult multi_replay(const SymbolTable& t, const std::vector<SymbolCommand>& commands, std::ostream& out,
                               const MultiReplayOptions& opt) {
    if (opt.check && opt.threads != 0) throw std::invalid_argument("check requires sequential mode (threads == 0); use verify");
    if (opt.verify && opt.threads == 0) throw std::invalid_argument("verify requires threads >= 1");
    for (const SymbolCommand& sc : commands)
        if (sc.symbol >= t.size()) throw std::invalid_argument("command refers to a symbol that is not in the table");

    std::vector<ReplayStats> per(t.size());
    MultiReplayResult result;

    if (opt.csv) out << "symbol," << csv_header() << '\n';

    if (opt.threads == 0) {
        result = opt.engine == EngineKind::Pooled ? sequential_replay<PoolPolicy>(t, commands, out, opt, per)
                                                  : sequential_replay<StdPolicy>(t, commands, out, opt, per);
    } else {
        PipelineOptions po;
        po.shards = opt.threads;
        po.queue_capacity = opt.queue_capacity;
        po.engine = opt.engine;
        po.journal = opt.journal;
        PipelineResult pr = run_pipeline(commands, po);

        std::size_t event_seq = 0;
        for (const SymbolEvent& se : pr.events) {
            tally_event(se.event, per[se.symbol]);
            emit_event(out, t, opt, event_seq, se.seq, se.symbol, se.event);
        }
        for (const SymbolCommand& sc : commands) ++per[sc.symbol].commands;  // SNAPSHOT lines count, as in sequential mode

        result.snapshots_skipped = pr.snapshots_skipped;

        if (opt.verify) {
            const PipelineResult ref = run_sequential(commands, po);
            std::optional<std::string> mismatch = diff_events(t, pr.events, ref.events);
            if (!mismatch) {
                bool same_books = pr.finals.size() == ref.finals.size();
                for (std::size_t i = 0; same_books && i < ref.finals.size(); ++i)
                    same_books = pr.finals[i].symbol == ref.finals[i].symbol && pr.finals[i].book == ref.finals[i].book &&
                                 pr.finals[i].resting == ref.finals[i].resting;
                if (!same_books) mismatch = "final books differ between threaded and sequential runs";
            }
            result.verify_mismatch = std::move(mismatch);
        }

        result.symbols.resize(t.size());
        for (SymbolFinal& f : pr.finals) {
            result.symbols[f.symbol].final_book = std::move(f.book);
            result.symbols[f.symbol].resting = f.resting;
        }
    }

    for (SymbolId s = 0; s < t.size(); ++s) {
        result.symbols[s].symbol = s;
        result.symbols[s].stats = per[s];
        merge_stats(result.total, per[s]);
        result.resting_orders += result.symbols[s].resting;
    }

    if (opt.csv) return result;  // pure CSV: no snapshots, no SUMMARY (the CLI reports it on stderr)
    if (opt.final_snapshot)
        for (const SymbolReplayResult& s : result.symbols)
            out << '@' << t.name(s.symbol) << ' ' << format_snapshot(s.final_book, opt.default_depth);  // truncates to depth
    for (const SymbolReplayResult& s : result.symbols) out << format_symbol_summary(t, s) << '\n';
    out << format_total_summary(t, result) << '\n';
    return result;
}

std::string format_symbol_summary(const SymbolTable& t, const SymbolReplayResult& r) {
    ReplayResult rr;
    rr.stats = r.stats;
    rr.resting_orders = r.resting;
    return "SUMMARY @" + t.name(r.symbol) + format_summary(rr).substr(7);  // drop the leading "SUMMARY"
}

std::string format_total_summary(const SymbolTable& t, const MultiReplayResult& r) {
    ReplayResult rr;
    rr.stats = r.total;
    rr.resting_orders = r.resting_orders;
    return "SUMMARY symbols=" + std::to_string(t.size()) + format_summary(rr).substr(7);
}

}  // namespace lob
