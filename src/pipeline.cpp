#include "lob/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include "lob/spsc_queue.hpp"

namespace lob {

namespace {

// What travels through a queue. Small and trivially copyable on purpose: the queue overwrites slots.
struct alignas(kCacheLine) WorkItem {
    std::uint64_t seq{};
    SymbolId symbol{};
    bool stop{false};  // sentinel: the worker returns after seeing it
    Command command;
};

// Escalating wait used by both ends of a queue: retry immediately, then yield the CPU, then sleep
// briefly. On a machine with fewer cores than threads the yield is what lets the other side run.
class Backoff {
public:
    void pause() {
        if (n_ < kSpin) {
            ++n_;
        } else if (n_ < kSpin + kYield) {
            ++n_;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    void reset() noexcept { n_ = 0; }

private:
    static constexpr unsigned kSpin = 32;
    static constexpr unsigned kYield = 1024;
    unsigned n_{0};
};

void push_blocking(SpscQueue<WorkItem>& q, const WorkItem& item) {
    Backoff b;
    while (!q.try_push(item)) b.pause();
}

template <class Policy>
void collect_finals(const BasicMultiEngine<Policy>& engines, std::vector<SymbolFinal>& out) {
    engines.for_each([&](SymbolId s, const typename BasicMultiEngine<Policy>::Engine& e) {
        out.push_back(SymbolFinal{s, e.book().snapshot(), e.book().order_count()});
    });
}

}  // namespace

// One worker thread and everything only it touches. alignas keeps two shards' hot fields apart.
template <class Policy>
struct alignas(kCacheLine) BasicShardedPipeline<Policy>::Shard {
    explicit Shard(std::size_t queue_capacity) : queue(queue_capacity) {}

    SpscQueue<WorkItem> queue;
    BasicMultiEngine<Policy> engines;  // created lazily BY the worker thread; read by finish() after join()
    std::vector<SymbolEvent> events;
    ReplayStats stats;
    std::size_t commands{0};
    std::exception_ptr error;
    std::thread thread;

    // The worker loop. On an exception it records the first one and keeps DRAINING (discarding) so the
    // producer can never block forever on a dead consumer; finish() rethrows after joining.
    void run(bool collect) {
        WorkItem item;
        std::vector<Event> scratch;
        Backoff idle;
        bool failed = false;
        for (;;) {
            if (!queue.try_pop(item)) {
                idle.pause();
                continue;
            }
            idle.reset();
            if (item.stop) return;
            if (failed) continue;
            try {
                scratch.clear();
                engines.apply(item.symbol, item.command, scratch);
                ++commands;
                for (const Event& e : scratch) {
                    tally_event(e, stats);
                    if (collect) events.push_back(SymbolEvent{item.seq, item.symbol, e});
                }
            } catch (...) {
                error = std::current_exception();
                failed = true;
            }
        }
    }
};

template <class Policy>
BasicShardedPipeline<Policy>::BasicShardedPipeline(const PipelineOptions& options) : options_(options) {
    if (options_.shards == 0) throw std::invalid_argument("PipelineOptions::shards must be >= 1");
    shards_.reserve(options_.shards);
    for (std::size_t i = 0; i < options_.shards; ++i) shards_.push_back(std::make_unique<Shard>(options_.queue_capacity));
    try {
        for (auto& s : shards_) s->thread = std::thread(&Shard::run, s.get(), options_.collect_events);
    } catch (...) {
        shutdown();  // stops the threads that did start; the rest are not joinable and are skipped
        throw;
    }
}

template <class Policy>
BasicShardedPipeline<Policy>::~BasicShardedPipeline() {
    shutdown();
}

template <class Policy>
void BasicShardedPipeline<Policy>::shutdown() noexcept {
    WorkItem stop;
    stop.stop = true;
    for (auto& s : shards_)
        if (s->thread.joinable()) push_blocking(s->queue, stop);
    for (auto& s : shards_)
        if (s->thread.joinable()) s->thread.join();
}

template <class Policy>
void BasicShardedPipeline<Policy>::submit(const SymbolCommand& command) {
    if (finished_) throw std::logic_error("submit() after finish()");
    const bool snapshot = std::holds_alternative<SnapshotCommand>(command.command);
    if (!snapshot && options_.journal) options_.journal(command);  // write-ahead: a throw here submits nothing
    const std::uint64_t seq = ++next_seq_;
    if (snapshot) {
        ++snapshots_skipped_;
        return;
    }
    WorkItem item;
    item.seq = seq;
    item.symbol = command.symbol;
    item.command = command.command;
    push_blocking(shards_[command.symbol % shards_.size()]->queue, item);
}

template <class Policy>
PipelineResult BasicShardedPipeline<Policy>::finish() {
    if (finished_) throw std::logic_error("finish() called twice");
    finished_ = true;
    shutdown();

    for (auto& s : shards_)
        if (s->error) std::rethrow_exception(s->error);

    PipelineResult r;
    r.snapshots_skipped = snapshots_skipped_;
    for (auto& s : shards_) {
        merge_stats(r.stats, s->stats);
        r.stats.commands += s->commands;
        r.shard_commands.push_back(s->commands);
        collect_finals(s->engines, r.finals);
    }
    std::sort(r.finals.begin(), r.finals.end(),
              [](const SymbolFinal& a, const SymbolFinal& b) { return a.symbol < b.symbol; });

    if (!options_.collect_events) return r;

    // K-way merge by input sequence number. Within a shard events are already in seq order, every
    // command went to exactly one shard (so seqs never tie across shards), and one command's events are
    // contiguous, so taking the whole run for the smallest head seq reproduces the sequential order.
    if (shards_.size() == 1) {
        r.events = std::move(shards_[0]->events);
        return r;
    }
    std::size_t total = 0;
    for (auto& s : shards_) total += s->events.size();
    r.events.reserve(total);
    std::vector<std::size_t> pos(shards_.size(), 0);
    for (;;) {
        std::size_t best = shards_.size();
        for (std::size_t i = 0; i < shards_.size(); ++i) {
            if (pos[i] >= shards_[i]->events.size()) continue;
            if (best == shards_.size() || shards_[i]->events[pos[i]].seq < shards_[best]->events[pos[best]].seq) best = i;
        }
        if (best == shards_.size()) break;
        auto& ev = shards_[best]->events;
        const std::uint64_t seq = ev[pos[best]].seq;
        while (pos[best] < ev.size() && ev[pos[best]].seq == seq) r.events.push_back(std::move(ev[pos[best]++]));
    }
    return r;
}

template class BasicShardedPipeline<StdPolicy>;
template class BasicShardedPipeline<PoolPolicy>;

namespace {

template <class Policy>
PipelineResult pipeline_impl(const std::vector<SymbolCommand>& commands, const PipelineOptions& options) {
    BasicShardedPipeline<Policy> p(options);
    for (const SymbolCommand& c : commands) p.submit(c);
    return p.finish();
}

template <class Policy>
PipelineResult sequential_impl(const std::vector<SymbolCommand>& commands, const PipelineOptions& options) {
    BasicMultiEngine<Policy> engines;
    std::vector<Event> scratch;
    PipelineResult r;
    std::uint64_t seq = 0;
    for (const SymbolCommand& c : commands) {
        ++seq;
        if (std::holds_alternative<SnapshotCommand>(c.command)) {
            ++r.snapshots_skipped;
            continue;
        }
        scratch.clear();
        engines.apply(c.symbol, c.command, scratch);
        ++r.stats.commands;
        for (const Event& e : scratch) {
            tally_event(e, r.stats);
            if (options.collect_events) r.events.push_back(SymbolEvent{seq, c.symbol, e});
        }
    }
    r.shard_commands.push_back(r.stats.commands);
    collect_finals(engines, r.finals);
    return r;
}

}  // namespace

PipelineResult run_pipeline(const std::vector<SymbolCommand>& commands, const PipelineOptions& options) {
    return options.engine == EngineKind::Pooled ? pipeline_impl<PoolPolicy>(commands, options)
                                                : pipeline_impl<StdPolicy>(commands, options);
}

PipelineResult run_sequential(const std::vector<SymbolCommand>& commands, const PipelineOptions& options) {
    return options.engine == EngineKind::Pooled ? sequential_impl<PoolPolicy>(commands, options)
                                                : sequential_impl<StdPolicy>(commands, options);
}

}  // namespace lob
