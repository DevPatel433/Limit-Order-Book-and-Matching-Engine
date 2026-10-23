// lob_mgen: write a seeded, reproducible multi-instrument replay file to stdout.
#include <charconv>
#include <cstring>
#include <iostream>
#include <string>

#include "lob/multi_generator.hpp"

namespace {

template <class T>
bool parse(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc() && p == end;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [--symbols N] [--hot-pct P] [--prefix NAME] [--seed S] [--count N]\n"
              << "          [--mid P] [--band B] [--max-qty Q] [--cancel-pct N] [--bad-pct N] [--sweep-pct N]\n"
              << "          [--snapshot-every N] [--ioc-pct N] [--fok-pct N] [--market-pct N] [--amend-pct N]\n"
              << "--count is the TOTAL number of commands; --hot-pct P sends P% of them to the first symbol.\n"
              << "Writes a multi-instrument replay file to stdout. Same options => byte-identical output.\n";
}

}  // namespace

int main(int argc, char** argv) {
    lob::MultiGeneratorConfig m;
    lob::GeneratorConfig& c = m.base;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) { usage(argv[0]); return 1; }
        const char* v = argv[++i];
        bool ok = false;
        if (a == "--symbols") ok = parse(v, m.symbols);
        else if (a == "--hot-pct") ok = parse(v, m.hot_percent);
        else if (a == "--prefix") { m.prefix = v; ok = true; }
        else if (a == "--seed") ok = parse(v, c.seed);
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
    lob::GeneratedMulti g;
    try {
        g = lob::generate_multi(m);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    std::cout << "# lob_mgen seed=" << c.seed << " count=" << c.count << " symbols=" << m.symbols << " hot_pct=" << m.hot_percent
              << " mid=" << c.mid << " band=" << c.band << " max_qty=" << c.max_quantity << " cancel_pct=" << c.cancel_percent
              << " bad_pct=" << c.bad_percent << " sweep_pct=" << c.sweep_percent << " snapshot_every=" << c.snapshot_every
              << " ioc_pct=" << c.ioc_percent << " fok_pct=" << c.fok_percent << " market_pct=" << c.market_percent
              << " amend_pct=" << c.amend_percent << '\n';
    lob::write_multi_commands(std::cout, g.symbols, g.commands);
    return 0;
}
