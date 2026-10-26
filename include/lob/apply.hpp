#pragma once

#include <type_traits>
#include <variant>
#include <vector>

#include "lob/commands.hpp"
#include "lob/events.hpp"

namespace lob {

// Runs one command against any engine, appending its events to `out`. SNAPSHOT produces no events
// (it is a view, not a state change). Shared by the replay runner and the interactive shell so the
// two can never disagree about what a command means.
template <class Engine>
void apply_command(Engine& engine, const Command& cmd, std::vector<Event>& out) {
    if (const auto* n = std::get_if<NewCommand>(&cmd)) engine.submit(n->order, out);
    else if (const auto* c = std::get_if<CancelCommand>(&cmd)) engine.cancel(c->id, out);
    else if (const auto* a = std::get_if<AmendCommand>(&cmd)) engine.amend(a->id, a->price, a->quantity, out);
}

}  // namespace lob
