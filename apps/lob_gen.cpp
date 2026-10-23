// lob_gen: write a seeded, reproducible random replay file to stdout.
#include <charconv>
#include <cstring>
#include <iostream>
#include <string>

#include "lob/generator.hpp"

namespace {

template <class T>
bool parse(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc() && p == end;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [--seed S] [--count N] [--mid P] [--band B] [--max-qty Q]\n"
              << "          [--cancel-pct N] [--bad-pct N] [--sweep-pct N] [--snapshot-every N]\n"
              << "          [--ioc-pct N] [--fok-pct N] [--market-pct N] [--amend-pct N]\n"
              << "Writes a replay file to stdout. Same options => byte-identical output.\n";
}

}  // namespace

int main(int argc, char** argv) {
    lob::GeneratorConfig c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) { usage(argv[0]); return 1; }
        const char* v = argv[++i];
        bool ok = false;
        if (a == "--seed") ok = parse(v, c.seed);
        else if (a == "--count") ok = parse(v, c.count);
        else if (a == "--mid") ok = parse(v, c.mid);
        else if (a == "--band") ok = parse(v, c.band);
        else if (a == "--max-qty") ok = parse(v, c.max_quantity);
        else if (a == "--cancel-pct") ok = parse(v, c.cancel_percent);
        else if (a == "--bad-pct") ok = parse(v, c.bad_percent);
        else if (a == "--sweep-pct") ok = parse(v, c.sweep_percent);
        else if (a == "--snapshot-every") ok = parse(v, c.snapshot_every);
        else if (a == "--ioc-pct") ok = parse(v, c.ioc_percent);
        else if (a == "--fok-pct") ok = parse(v, c.fok_percent);
        else if (a == "--market-pct") ok = parse(v, c.market_percent);
        else if (a == "--amend-pct") ok = parse(v, c.amend_percent);
        if (!ok) { usage(argv[0]); return 1; }
    }
    std::cout << "# lob_gen seed=" << c.seed << " count=" << c.count << " mid=" << c.mid << " band=" << c.band
              << " max_qty=" << c.max_quantity << " cancel_pct=" << c.cancel_percent << " bad_pct=" << c.bad_percent
              << " sweep_pct=" << c.sweep_percent << " snapshot_every=" << c.snapshot_every;
    // Only echoed when used, so headers (and files) for older option sets are unchanged.
    if (c.ioc_percent || c.fok_percent || c.market_percent || c.amend_percent)
        std::cout << " ioc_pct=" << c.ioc_percent << " fok_pct=" << c.fok_percent << " market_pct=" << c.market_percent
                  << " amend_pct=" << c.amend_percent;
    std::cout << '\n';
    lob::write_commands(std::cout, lob::generate(c));
    return 0;
}
