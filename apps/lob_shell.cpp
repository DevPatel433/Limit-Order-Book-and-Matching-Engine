// lob_shell: a tiny interactive terminal view of the matching engine.
//
//   $ ./build/lob_shell
//   > NEW 1 SELL 101 5
//   > NEW 2 BUY MKT 3
//   > AMEND 1 100 5
//   > book
//
// Type replay-file commands (docs/replay_format.md); after each one the events and the order book are
// shown. Extra commands: book [depth], help, quit. Pipe a file in for a non-interactive run.
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

#include "lob/apply.hpp"
#include "lob/commands.hpp"
#include "lob/matching_engine.hpp"
#include "lob/replay.hpp"

namespace {

void help() {
    std::cout << "commands:\n"
                 "  NEW <id> <BUY|SELL> <price|MKT> <qty> [GTC|IOC|FOK]\n"
                 "  CANCEL <id>\n"
                 "  AMEND <id> <price> <qty>\n"
                 "  book [depth]   show the book      help      quit\n";
}

template <class Engine>
int run(std::size_t depth, bool interactive) {
    Engine engine;
    std::vector<lob::Event> events;
    std::string line;
    if (interactive) help();
    while (true) {
        if (interactive) std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const std::string word = line.substr(first, line.find_first_of(" \t\r", first) - first);
        if (word == "quit" || word == "exit") break;
        if (word == "help") { help(); continue; }
        if (word == "book") {
            std::size_t d = depth;
            if (line.size() > first + 4) d = static_cast<std::size_t>(std::strtoull(line.c_str() + first + 4, nullptr, 10));
            std::cout << lob::format_snapshot(engine.book().snapshot(d), d);
            continue;
        }

        const lob::ParseResult parsed = lob::parse_commands(line);
        if (!parsed.ok()) {
            std::cout << "error: " << parsed.error->message << "  (type 'help')\n";
            continue;
        }
        for (const lob::Command& cmd : parsed.commands) {
            events.clear();
            lob::apply_command(engine, cmd, events);
            for (const lob::Event& e : events) std::cout << "  " << lob::to_string(e) << '\n';
            if (const auto* snap = std::get_if<lob::SnapshotCommand>(&cmd)) {
                const std::size_t d = snap->depth.value_or(depth);
                std::cout << lob::format_snapshot(engine.book().snapshot(d), d);
            }
        }
        std::cout << lob::format_snapshot(engine.book().snapshot(depth), depth);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool pooled = false;
    std::size_t depth = 5;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--engine") && i + 1 < argc) pooled = !std::strcmp(argv[++i], "pooled");
        else if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) depth = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else {
            std::cerr << "usage: lob_shell [--engine baseline|pooled] [--depth N]\n";
            return 1;
        }
    }
    const bool interactive = isatty(STDIN_FILENO) != 0;
    return pooled ? run<lob::PooledMatchingEngine>(depth, interactive) : run<lob::MatchingEngine>(depth, interactive);
}
