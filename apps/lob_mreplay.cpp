// lob_mreplay: run a multi-instrument replay file, sequentially or on sharded worker threads.
#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "lob/multi_replay.hpp"
#include "lob/wal.hpp"

namespace {

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " <file|-> [--threads N] [--queue-capacity C] [--verify] [--check]\n"
              << "                   [--csv] [--quiet] [--no-final] [--depth N] [--engine baseline|pooled]\n"
              << "                   [--default-symbol S] [--wal FILE] [--wal-sync N]\n"
              << "  <file|->            multi-instrument replay file (see docs/replay_format.md); '-' reads stdin\n"
              << "  --threads N         run the sharded pipeline on N worker threads (default 0: sequential)\n"
              << "  --queue-capacity C  per-shard queue size, rounded up to a power of two (default 4096)\n"
              << "  --verify            with --threads: also run sequentially and require identical results\n"
              << "  --check             sequential only: verify every command with an independent checker per symbol\n"
              << "  --csv               stdout is a pure CSV event log (leading `symbol` column); summary on stderr\n"
              << "  --quiet             do not print event lines\n"
              << "  --no-final          do not print the final books\n"
              << "  --depth N           levels per side for SNAPSHOT and the final books (default 5)\n"
              << "  --engine E          implementation per symbol (default baseline); output is identical\n"
              << "  --default-symbol S  route lines that have no @SYMBOL prefix to S (default: such lines are errors)\n"
              << "  --wal FILE          write-ahead log of the input (created; fails if FILE exists); recover with lob_recover\n"
              << "  --wal-sync N        fsync the log every N records (default 0 = never; 1 = every record)\n"
              << "Threaded mode skips SNAPSHOT commands (final books are still printed).\n"
              << "exit codes: 0 ok, 1 usage/IO/parse error, 2 consistency violation or --verify mismatch\n";
}

template <class T>
bool parse_num(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc() && p == end;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    lob::MultiReplayOptions opt;
    std::optional<std::string> default_symbol;
    std::string wal_path;
    lob::WalOptions wal_options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quiet") opt.print_events = false;
        else if (a == "--check") opt.check = true;
        else if (a == "--verify") opt.verify = true;
        else if (a == "--no-final") opt.final_snapshot = false;
        else if (a == "--csv") opt.csv = true;
        else if (a == "--engine" && i + 1 < argc) {
            const std::string e = argv[++i];
            if (e == "baseline") opt.engine = lob::EngineKind::Baseline;
            else if (e == "pooled") opt.engine = lob::EngineKind::Pooled;
            else { usage(argv[0]); return 1; }
        } else if (a == "--threads" && i + 1 < argc) {
            if (!parse_num(argv[++i], opt.threads)) { usage(argv[0]); return 1; }
        } else if (a == "--queue-capacity" && i + 1 < argc) {
            if (!parse_num(argv[++i], opt.queue_capacity)) { usage(argv[0]); return 1; }
        } else if (a == "--depth" && i + 1 < argc) {
            if (!parse_num(argv[++i], opt.default_depth)) { usage(argv[0]); return 1; }
        } else if (a == "--wal" && i + 1 < argc) {
            wal_path = argv[++i];
        } else if (a == "--wal-sync" && i + 1 < argc) {
            if (!parse_num(argv[++i], wal_options.sync_every)) { usage(argv[0]); return 1; }
        } else if (a == "--default-symbol" && i + 1 < argc) {
            default_symbol = argv[++i];
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            usage(argv[0]);
            return 1;
        } else if (path.empty()) path = a;
        else { usage(argv[0]); return 1; }
    }
    if (path.empty()) { usage(argv[0]); return 1; }
    if (opt.check && opt.threads != 0) { std::cerr << "error: --check needs sequential mode; use --verify with --threads\n"; return 1; }
    if (opt.verify && opt.threads == 0) { std::cerr << "error: --verify needs --threads N (N >= 1)\n"; return 1; }

    lob::MultiParseResult parsed;
    if (path == "-") {
        parsed = lob::parse_multi_commands(std::cin, default_symbol);
    } else {
        std::ifstream in(path);
        if (!in) { std::cerr << "error: cannot open " << path << '\n'; return 1; }
        parsed = lob::parse_multi_commands(in, default_symbol);
    }
    if (!parsed.ok()) {
        std::cerr << "error: " << path << ":" << parsed.error->line << ": " << parsed.error->message << '\n';
        return 1;
    }

    std::optional<lob::WalWriter> wal;
    lob::MultiReplayResult result;
    try {
        if (!wal_path.empty()) {
            wal.emplace(lob::WalWriter::create(wal_path, wal_options));
            opt.journal = [&](const lob::SymbolCommand& sc) { wal->append(parsed.symbols.name(sc.symbol), sc.command); };
        }
        result = lob::multi_replay(parsed.symbols, parsed.commands, std::cout, opt);
        if (wal) wal->close();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    if (result.violation) {
        std::cerr << "CONSISTENCY VIOLATION at command " << result.violation->command_index << ": "
                  << result.violation->message << '\n';
        return 2;
    }
    if (result.verify_mismatch) {
        std::cerr << "VERIFY MISMATCH: " << *result.verify_mismatch << '\n';
        return 2;
    }
    if (wal) std::cerr << "wal: " << wal->last_seq() << " records, " << wal->bytes_written() << " bytes -> " << wal_path << '\n';
    if (opt.csv) std::cerr << lob::format_total_summary(parsed.symbols, result) << '\n';
    if (result.snapshots_skipped) std::cerr << "note: threaded mode skipped " << result.snapshots_skipped << " SNAPSHOT command(s)\n";
    if (opt.check) std::cerr << "check passed: " << result.total.commands << " commands across " << parsed.symbols.size() << " symbols\n";
    if (opt.verify) std::cerr << "verify passed: threaded output identical to sequential (" << opt.threads << " threads)\n";
    return 0;
}
