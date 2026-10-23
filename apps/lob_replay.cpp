// lob_replay: run a replay file through the matching engine and print the result.
#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "lob/replay.hpp"

namespace {

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " <file|-> [--depth N] [--quiet] [--check] [--no-final]\n"
              << "                  [--engine baseline|pooled] [--csv]\n"
              << "  <file|->    replay file (see docs/replay_format.md); '-' reads stdin\n"
              << "  --depth N   default depth for SNAPSHOT and the final book (default 5)\n"
              << "  --quiet     do not print event lines (snapshots and SUMMARY only)\n"
              << "  --check     verify every command with the independent consistency checker\n"
              << "  --no-final  do not print the final book\n"
              << "  --engine E  implementation to run (default baseline); output is identical\n"
              << "  --csv       stdout is a pure CSV event log (header + one row per event); summary on stderr\n"
              << "exit codes: 0 ok, 1 usage/IO/parse error, 2 consistency violation\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    lob::ReplayOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quiet") opt.print_events = false;
        else if (a == "--check") opt.check = true;
        else if (a == "--no-final") opt.final_snapshot = false;
        else if (a == "--csv") opt.csv = true;
        else if (a == "--engine" && i + 1 < argc) {
            const std::string e = argv[++i];
            if (e == "baseline") opt.engine = lob::EngineKind::Baseline;
            else if (e == "pooled") opt.engine = lob::EngineKind::Pooled;
            else { usage(argv[0]); return 1; }
        }
        else if (a == "--depth" && i + 1 < argc) {
            const char* s = argv[++i];
            const char* end = s + std::strlen(s);
            auto [p, ec] = std::from_chars(s, end, opt.default_depth);
            if (ec != std::errc() || p != end) { usage(argv[0]); return 1; }
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            usage(argv[0]);
            return 1;
        } else if (path.empty()) path = a;
        else { usage(argv[0]); return 1; }
    }
    if (path.empty()) { usage(argv[0]); return 1; }

    lob::ParseResult parsed;
    if (path == "-") {
        parsed = lob::parse_commands(std::cin);
    } else {
        std::ifstream in(path);
        if (!in) { std::cerr << "error: cannot open " << path << '\n'; return 1; }
        parsed = lob::parse_commands(in);
    }
    if (!parsed.ok()) {
        std::cerr << "error: " << path << ":" << parsed.error->line << ": " << parsed.error->message << '\n';
        return 1;
    }

    const lob::ReplayResult result = lob::replay(parsed.commands, std::cout, opt);
    if (result.violation) {
        std::cerr << "CONSISTENCY VIOLATION at command " << result.violation->command_index << ": "
                  << result.violation->message << '\n';
        return 2;
    }
    if (opt.csv) std::cerr << lob::format_summary(result) << '\n';
    if (opt.check) std::cerr << "check passed: " << result.stats.commands << " commands\n";
    return 0;
}
