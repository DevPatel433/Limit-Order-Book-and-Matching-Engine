#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lob/multi.hpp"
#include "lob/replay.hpp"

namespace lob {

// Phase 8: a write-ahead log (WAL) of the SEQUENCED INPUT, and crash recovery by deterministic replay.
//
// Why log inputs, not state: the engine is a deterministic function of its command sequence (Phases 2-7
// test exactly that), so the ordered commands are a complete, compact description of every book. Recovery
// is "replay the log". Format and guarantees: docs/wal_format.md, rationale: docs/phase8_design.md.
//
// File = 8-byte magic, then records. Every record starts with a CRC-32 over the rest of the record, then a
// kind byte. All integers little-endian, so the file is portable across platforms.

inline constexpr char kWalMagic[8] = {'L', 'O', 'B', 'W', 'A', 'L', '0', '1'};
inline constexpr std::size_t kWalHeaderBytes = sizeof(kWalMagic);
inline constexpr std::size_t kWalMaxRecordBytes = 44;  // the largest record (NEW): crc 4 + kind 1 + 39

// Standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). crc32("123456789") == 0xCBF43926.
// `crc` chains: wal_crc32(b, nb, wal_crc32(a, na)) == crc of a followed by b.
std::uint32_t wal_crc32(const void* data, std::size_t size, std::uint32_t crc = 0) noexcept;

enum class WalStatus : std::uint8_t {
    Clean,      // every byte of the file belongs to a valid record
    TornTail,   // the file ends in an incomplete or checksum-failed FINAL record (a crash mid-write);
                // everything before it is valid and recoverable
    Corrupt,    // damage that cannot be a torn write: bad data followed by more data, or valid-looking
                // records that violate the log's invariants (sequence gap, undefined symbol, ...)
    BadHeader,  // a complete 8-byte header that is not kWalMagic: not a WAL file (or another version)
};
const char* to_string(WalStatus s) noexcept;

struct WalScan {
    WalStatus status{WalStatus::Clean};
    std::uint64_t file_bytes{};   // size of the file as scanned
    std::uint64_t valid_bytes{};  // length of the valid prefix: header plus every good record
    std::uint64_t commands{};     // command records in the valid prefix
    std::uint64_t last_seq{};     // sequence number of the last good command record (0 if none)
    std::vector<std::string> symbols;  // names in id order (ids are dense, by first logged use)
    std::string message;          // human-readable detail for anything but Clean
    bool usable() const noexcept { return status == WalStatus::Clean || status == WalStatus::TornTail; }
};

// Called once per good command record, in log order. `seq` counts logged commands from 1.
using WalVisitor = std::function<void(std::uint64_t seq, const std::string& symbol, const Command& command)>;

// Reads the log sequentially, calling `visit` for every valid command up to the first problem, then
// classifies that problem. NEVER throws for bad content (that is what the status is for).
//   * An empty or partial header, or a record cut short by end-of-file, is a TornTail.
//   * A checksum failure on the LAST record is a TornTail; on any earlier record it is Corrupt.
//   * Unknown kind, over-long symbol name: Corrupt, unless the record starts within the last
//     kWalMaxRecordBytes of the file (then it cannot be told from a torn write, and is a TornTail).
//   * Valid checksum but a broken invariant (seq != previous + 1, symbol id undefined or not dense,
//     duplicate or invalid symbol name, invalid side/TIF/type): always Corrupt.
// Consequence, stated plainly: damage inside the final ~44 bytes (about the last record) is treated as a
// torn write, because a torn write looks exactly like that. Damage anywhere earlier is detected.
WalScan wal_scan(std::istream& in, const WalVisitor& visit);
// Throws std::runtime_error only if the file cannot be opened.
WalScan wal_scan_file(const std::string& path, const WalVisitor& visit);

struct WalOptions {
    // 0: never fsync (data may sit in user-space buffers; survives nothing but a clean close).
    // 1: flush + fsync after every record (a command is durable before apply() returns).
    // N: flush + fsync every N records (a crash can lose up to N-1 acknowledged commands).
    std::size_t sync_every{0};
};

// Appends records. Not thread-safe (the sequencer is the single writer). Any I/O error throws
// std::runtime_error and POISONS the writer: every later call throws, so a half-written record can never
// be followed by more data (which would turn a torn tail into mid-log corruption).
class WalWriter {
public:
    // New log; fails (std::runtime_error) if the file already exists, so a log is never clobbered.
    static WalWriter create(const std::string& path, const WalOptions& options = {});
    // Resume an existing log: scan it, truncate a torn tail, and continue appending with the next
    // sequence number. Throws std::runtime_error for a Corrupt or BadHeader file (never "repairs" those).
    // If `scan_out` is set it receives what was found.
    static WalWriter open_append(const std::string& path, const WalOptions& options = {}, WalScan* scan_out = nullptr);
    // Wraps an already-open stream that is positioned after a valid header (used by tests to inject I/O
    // failures with /dev/full). Takes ownership.
    static WalWriter from_file(std::FILE* file, const WalOptions& options = {}, std::uint64_t last_seq = 0);

    WalWriter(WalWriter&&) noexcept = default;
    WalWriter& operator=(WalWriter&&) noexcept = default;
    ~WalWriter();

    // Logs one command for `symbol`, defining the symbol first if it is new to this log. Returns the
    // symbol's log-local id. Throws std::invalid_argument for an invalid symbol name or a SNAPSHOT (a view,
    // not a state change, so never logged) BEFORE writing anything.
    SymbolId append(std::string_view symbol, const Command& command);

    void sync();   // flush and fsync now
    void close();  // flush and close; idempotent. The destructor closes best-effort and never throws.

    std::uint64_t last_seq() const noexcept { return last_seq_; }
    std::uint64_t bytes_written() const noexcept { return bytes_; }  // file offset after the last record

private:
    struct FileCloser {
        void operator()(std::FILE* f) const noexcept;
    };
    WalWriter() = default;
    void write_record(const unsigned char* data, std::size_t size);
    void note_synced_or_sync();

    std::unique_ptr<std::FILE, FileCloser> file_;
    WalOptions options_;
    std::map<std::string, SymbolId, std::less<>> ids_;
    std::uint64_t last_seq_{0};
    std::uint64_t bytes_{0};
    std::size_t since_sync_{0};
    bool poisoned_{false};
};

// ---- Recovery ------------------------------------------------------------------------------------

template <class Policy>
struct BasicRecovered {
    WalScan scan;
    SymbolTable symbols;                // log-local ids, by first logged use
    BasicMultiEngine<Policy> engines;   // the books exactly as they were after the last good command
    ReplayStats stats;                  // events produced while replaying (commands = replayed commands)
};

// Rebuilds all books by replaying every valid command. For TornTail the result is the state after the
// last complete record; for Corrupt it is the state just BEFORE the damage and `scan.status` says so (the
// caller decides whether a partial state is acceptable; the library never hides it). Throws only if the
// file cannot be opened.
template <class Policy>
BasicRecovered<Policy> recover_wal(const std::string& path);

extern template BasicRecovered<StdPolicy> recover_wal<StdPolicy>(const std::string&);
extern template BasicRecovered<PoolPolicy> recover_wal<PoolPolicy>(const std::string&);

// Write-ahead engine: every command is appended to the log BEFORE it is applied, so after a crash the
// log is never behind the books. If the append throws, the command was not applied.
template <class Policy>
class DurableMultiEngine {
public:
    DurableMultiEngine(WalWriter writer, SymbolTable symbols, BasicMultiEngine<Policy> engines)
        : writer_(std::move(writer)), symbols_(std::move(symbols)), engines_(std::move(engines)) {}

    // Creates the log if it does not exist; otherwise recovers it (throws if Corrupt/BadHeader), truncates a
    // torn tail, and resumes. `scan_out` receives what recovery found (a fresh log reports Clean, 0 commands).
    static DurableMultiEngine open(const std::string& path, const WalOptions& options = {}, WalScan* scan_out = nullptr);

    // Logs, then applies. SNAPSHOT commands are neither logged nor applied (no events).
    void apply(std::string_view symbol, const Command& command, std::vector<Event>& out);

    void sync() { writer_.sync(); }
    void close() { writer_.close(); }
    const WalWriter& wal() const noexcept { return writer_; }
    const SymbolTable& symbols() const noexcept { return symbols_; }
    const BasicMultiEngine<Policy>& engines() const noexcept { return engines_; }

private:
    WalWriter writer_;
    SymbolTable symbols_;
    BasicMultiEngine<Policy> engines_;
};

using DurableEngine = DurableMultiEngine<StdPolicy>;
using PooledDurableEngine = DurableMultiEngine<PoolPolicy>;

}  // namespace lob

#include "lob/wal_impl.hpp"
