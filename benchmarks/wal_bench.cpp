// lob_wal: what does the write-ahead log cost, and how fast is recovery?
//
// Methodology (docs/results_phase8.md): one pre-generated workload (generation never timed); modes are
// INTERLEAVED inside every repetition; median of R repetitions; timing covers journalling (when on),
// matching and, for the file modes, the final flush/close. Events are discarded (the question is the log's
// cost, not event storage). Recovery is timed on the log the same run just wrote: scan + decode + replay.
// The log lives in --dir (default: the system temp dir); fsync cost depends entirely on the storage under
// that directory, so the directory is recorded and the fsync rows must not be generalised.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "environment.hpp"
#include "lob/multi_generator.hpp"
#include "lob/wal.hpp"

using namespace lob;
namespace fs = std::filesystem;

namespace {

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
template <class T>
bool parse(const std::string& s, T& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

struct Mode {
    std::string name;
    bool wal;
    std::size_t sync_every;
    std::size_t count;  // commands run in this mode
};

// Runs `count` commands; optionally journals each one first. Returns seconds, fills `stats`.
double run_mode(const GeneratedMulti& g, const Mode& m, const std::string& path, ReplayStats& stats, std::uint64_t& bytes) {
    MultiEngine engines;
    std::vector<Event> scratch;
    stats = ReplayStats{};
    bytes = 0;
    const double t0 = now();
    if (m.wal) {
        WalOptions o;
        o.sync_every = m.sync_every;
        WalWriter w = WalWriter::create(path, o);
        for (std::size_t i = 0; i < m.count; ++i) {
            const SymbolCommand& c = g.commands[i];
            w.append(g.symbols.name(c.symbol), c.command);  // write-ahead
            scratch.clear();
            engines.apply(c.symbol, c.command, scratch);
            for (const Event& e : scratch) tally_event(e, stats);
        }
        w.close();
        bytes = w.bytes_written();
    } else {
        for (std::size_t i = 0; i < m.count; ++i) {
            const SymbolCommand& c = g.commands[i];
            scratch.clear();
            engines.apply(c.symbol, c.command, scratch);
            for (const Event& e : scratch) tally_event(e, stats);
        }
    }
    return now() - t0;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t count = 500000, sync1_count = 20000, symbols = 16;
    int reps = 5;
    std::string dir = fs::temp_directory_path().string(), out;
    bool allow_debug = false, smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--allow-debug") { allow_debug = true; continue; }
        if (a == "--smoke") { smoke = true; continue; }
        if (i + 1 >= argc) { std::cerr << "usage: lob_wal [--count N] [--sync1-count N] [--symbols N] [--reps R] [--dir PATH] [--out FILE] [--smoke] [--allow-debug]\n"; return 1; }
        const std::string v = argv[++i];
        bool ok = true;
        if (a == "--count") ok = parse(v, count);
        else if (a == "--sync1-count") ok = parse(v, sync1_count);
        else if (a == "--symbols") ok = parse(v, symbols);
        else if (a == "--reps") ok = parse(v, reps);
        else if (a == "--dir") dir = v;
        else if (a == "--out") out = v;
        else ok = false;
        if (!ok) { std::cerr << "bad argument: " << a << '\n'; return 1; }
    }
    if (smoke) { count = 5000; sync1_count = 500; reps = 2; symbols = 4; }
    sync1_count = std::min(sync1_count, count);

    bench::Environment env = bench::collect_environment();
    if (!env.valid_for_comparison() && !allow_debug) {
        std::cerr << "Refusing to run: assertions or sanitizers are enabled, so timings are meaningless (use Release, or --allow-debug).\n";
        return 1;
    }
    if (!fs::is_directory(dir)) { std::cerr << "--dir is not a directory: " << dir << '\n'; return 1; }

    MultiGeneratorConfig mg;
    mg.base.seed = 777;
    mg.base.count = count;
    mg.base.band = 5;
    mg.base.cancel_percent = 20;
    mg.base.ioc_percent = 5;
    mg.base.market_percent = 3;
    mg.base.amend_percent = 5;
    mg.symbols = symbols;
    const GeneratedMulti g = generate_multi(mg);

    const std::vector<Mode> modes = {
        {"no log", false, 0, count},          {"log, never fsync", true, 0, count},
        {"log, fsync/1000", true, 1000, count}, {"log, fsync/100", true, 100, count},
        {"no log (small)", false, 0, sync1_count}, {"log, fsync/1 (small)", true, 1, sync1_count},
    };

    const std::string path = (fs::path(dir) / ("lob_wal_bench_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::map<std::string, std::vector<double>> secs;
    std::map<std::string, std::uint64_t> log_bytes;
    std::vector<double> recover_secs;
    std::uint64_t recover_cmds = 0;
    std::ostringstream csv;
    csv << "# lob_wal: commands=" << count << " small_commands=" << sync1_count << " symbols=" << symbols << " reps=" << reps << " dir=" << dir << '\n'
        << "# cpu=" << env.cpu_model << " logical_cpus=" << env.logical_cpus << " os=" << env.os << " compiler=" << env.compiler << '\n'
        << "# build_type=" << env.build_type << " flags=" << env.cxx_flags << " valid_for_comparison=" << (env.valid_for_comparison() ? 1 : 0) << " timestamp_utc=" << env.timestamp_utc << '\n'
        << "mode,commands,rep,seconds,commands_per_sec,log_bytes\n";

    std::map<std::size_t, ReplayStats> ref;  // per command count, from the no-log run: the correctness gate
    for (int rep = 0; rep < reps; ++rep) {
        for (const Mode& m : modes) {
            std::error_code ec;
            fs::remove(path, ec);
            ReplayStats st;
            std::uint64_t bytes = 0;
            const double s = run_mode(g, m, path, st, bytes);
            if (!m.wal) ref[m.count] = st;
            else if (ref.count(m.count) && (st.trades != ref[m.count].trades || st.traded_quantity != ref[m.count].traded_quantity || st.accepted != ref[m.count].accepted)) {
                std::cerr << "CORRECTNESS GATE FAILED: journalling changed the results in mode " << m.name << '\n';
                return 2;
            }
            secs[m.name].push_back(s);
            if (m.wal) log_bytes[m.name] = bytes;
            csv << '"' << m.name << "\"," << m.count << ',' << rep << ',' << std::fixed << std::setprecision(6) << s << ',' << std::setprecision(0)
                << static_cast<double>(m.count) / s << ',' << bytes << '\n';

            if (m.name == "log, never fsync") {  // time recovery of the log we just wrote, and check it
                const double r0 = now();
                const auto rec = recover_wal<StdPolicy>(path);
                recover_secs.push_back(now() - r0);
                recover_cmds = rec.scan.commands;
                if (rec.scan.status != WalStatus::Clean || rec.stats.trades != st.trades || rec.stats.traded_quantity != st.traded_quantity) {
                    std::cerr << "CORRECTNESS GATE FAILED: recovery did not reproduce the run\n";
                    return 2;
                }
            }
        }
    }
    std::error_code ec;
    fs::remove(path, ec);

    if (out.empty()) std::cout << csv.str();
    else { std::ofstream(out) << csv.str(); std::cout << "wrote " << out << '\n'; }

    std::cout << "\nlob_wal: " << count << " commands over " << symbols << " symbols, " << reps << " reps, log dir " << dir << ", logical CPUs " << env.logical_cpus << "\n\n"
              << std::left << std::setw(24) << "mode" << std::right << std::setw(10) << "commands" << std::setw(12) << "median s" << std::setw(15) << "commands/s"
              << std::setw(12) << "vs no log" << std::setw(10) << "B/cmd" << '\n';
    for (const Mode& m : modes) {
        const std::string base_name = m.count == count ? "no log" : "no log (small)";
        const double med = median(secs[m.name]), base = median(secs[base_name]);
        std::cout << std::left << std::setw(24) << m.name << std::right << std::setw(10) << m.count << std::fixed << std::setw(12) << std::setprecision(4) << med
                  << std::setw(15) << std::setprecision(0) << static_cast<double>(m.count) / med << std::setw(11) << std::setprecision(2) << med / base << 'x'
                  << std::setw(10) << std::setprecision(1) << (m.wal ? static_cast<double>(log_bytes[m.name]) / static_cast<double>(m.count) : 0.0) << '\n';
    }
    const double rec = median(recover_secs);
    std::cout << "\nrecovery of the " << recover_cmds << "-command log (scan + CRC + decode + replay): median " << std::setprecision(4) << rec << " s = "
              << std::setprecision(0) << static_cast<double>(recover_cmds) / rec << " commands/s\n"
              << "'vs no log' is the slowdown factor (time ratio, same command count). fsync rows depend on the storage under --dir.\n";
    return 0;
}
