// lob_recover: rebuild every book from a write-ahead log and print the result.
#include <charconv>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "lob/multi_replay.hpp"
#include "lob/wal.hpp"

namespace {

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " <wal-file> [--depth N] [--engine baseline|pooled] [--truncate] [--quiet]\n"
              << "  --depth N     levels per side to print for each book (default 5)\n"
              << "  --engine E    implementation to rebuild into (default baseline); the result is identical\n"
              << "  --truncate    if the log ends in a torn record, cut the file back to its valid prefix\n"
              << "  --quiet       do not print the books\n"
              << "Prints each recovered book as `@SYMBOL BOOK ...` (same text as lob_mreplay's final books).\n"
              << "exit codes: 0 clean, 3 recovered after discarding a torn tail, 2 corrupt or not a log, 1 usage/IO error\n";
}

template <class Policy>
int run(const std::string& path, std::size_t depth, bool truncate, bool quiet) {
    lob::BasicRecovered<Policy> rec = lob::recover_wal<Policy>(path);
    const lob::WalScan& s = rec.scan;

    if (!quiet && s.usable()) {
        rec.engines.for_each([&](lob::SymbolId id, const typename lob::BasicMultiEngine<Policy>::Engine& e) {
            std::cout << '@' << rec.symbols.name(id) << ' ' << lob::format_snapshot(e.book().snapshot(depth), depth);
        });
    }
    std::cerr << "log: " << lob::to_string(s.status) << ", " << s.commands << " commands, " << s.symbols.size()
              << " symbols, last_seq=" << s.last_seq << ", valid " << s.valid_bytes << " of " << s.file_bytes << " bytes\n";
    if (!s.message.empty()) std::cerr << "detail: " << s.message << '\n';

    if (s.status == lob::WalStatus::Corrupt || s.status == lob::WalStatus::BadHeader) {
        std::cerr << "refusing to treat a damaged log as recovered; books above (if any) are the state just BEFORE the damage\n";
        return 2;
    }
    if (s.status == lob::WalStatus::TornTail) {
        std::cerr << "discarded " << (s.file_bytes - s.valid_bytes) << " bytes of torn tail (a crash during the last write)\n";
        if (truncate) {
            std::filesystem::resize_file(path, s.valid_bytes);
            std::cerr << "truncated " << path << " to " << s.valid_bytes << " bytes\n";
        }
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::size_t depth = 5;
    bool truncate = false, quiet = false;
    lob::EngineKind engine = lob::EngineKind::Baseline;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--truncate") truncate = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--engine" && i + 1 < argc) {
            const std::string e = argv[++i];
            if (e == "baseline") engine = lob::EngineKind::Baseline;
            else if (e == "pooled") engine = lob::EngineKind::Pooled;
            else { usage(argv[0]); return 1; }
        } else if (a == "--depth" && i + 1 < argc) {
            const char* s = argv[++i];
            const char* end = s + std::strlen(s);
            auto [p, ec] = std::from_chars(s, end, depth);
            if (ec != std::errc() || p != end) { usage(argv[0]); return 1; }
        } else if (!a.empty() && a[0] == '-') { usage(argv[0]); return 1; }
        else if (path.empty()) path = a;
        else { usage(argv[0]); return 1; }
    }
    if (path.empty()) { usage(argv[0]); return 1; }
    try {
        return engine == lob::EngineKind::Pooled ? run<lob::PoolPolicy>(path, depth, truncate, quiet)
                                                 : run<lob::StdPolicy>(path, depth, truncate, quiet);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
