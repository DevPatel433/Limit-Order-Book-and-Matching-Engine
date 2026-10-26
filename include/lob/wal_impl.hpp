#pragma once
// Template definitions for DurableMultiEngine (included by wal.hpp; not a public header).

#include <filesystem>
#include <stdexcept>

namespace lob {

template <class Policy>
DurableMultiEngine<Policy> DurableMultiEngine<Policy>::open(const std::string& path, const WalOptions& options,
                                                           WalScan* scan_out) {
    if (!std::filesystem::exists(path)) {
        if (scan_out) *scan_out = WalScan{};
        return DurableMultiEngine(WalWriter::create(path, options), SymbolTable{}, BasicMultiEngine<Policy>{});
    }
    BasicRecovered<Policy> rec = recover_wal<Policy>(path);
    if (!rec.scan.usable()) throw std::runtime_error("cannot resume log: " + std::string(to_string(rec.scan.status)) + ": " + rec.scan.message);
    WalWriter w = WalWriter::open_append(path, options, scan_out);  // truncates a torn tail
    return DurableMultiEngine(std::move(w), std::move(rec.symbols), std::move(rec.engines));
}

template <class Policy>
void DurableMultiEngine<Policy>::apply(std::string_view symbol, const Command& command, std::vector<Event>& out) {
    if (std::holds_alternative<SnapshotCommand>(command)) return;
    const SymbolId logged = writer_.append(symbol, command);  // write-ahead: throws before any state change
    const SymbolId id = symbols_.intern(symbol);
    if (id != logged) throw std::logic_error("symbol table and log disagree on a symbol id");
    engines_.apply(id, command, out);
}

}  // namespace lob
