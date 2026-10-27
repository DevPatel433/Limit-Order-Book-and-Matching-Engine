// Phase 8 tests: the write-ahead log, crash recovery, and the write-ahead guarantee.
// Central properties (docs/phase8_design.md):
//   * recovery after ANY truncation of the log equals the exact state after some prefix of the commands;
//   * damage before the final ~44 bytes is detected, never silently dropped;
//   * recovered state equals the state of the uninterrupted run.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

#include "lob/multi_generator.hpp"
#include "lob/multi_replay.hpp"
#include "lob/pipeline.hpp"
#include "lob/wal.hpp"

using namespace lob;
namespace fs = std::filesystem;

namespace {

struct TempPath {
    fs::path path;
    explicit TempPath(const std::string& name) {
        path = fs::temp_directory_path() / ("lob_wal_" + name + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove(path);
    }
    ~TempPath() { std::error_code ec; fs::remove(path, ec); }
    std::string str() const { return path.string(); }
};

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}
void spit(const std::string& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

GeneratedMulti busy(std::uint64_t seed, std::size_t count, std::size_t symbols, Price band = 4) {
    MultiGeneratorConfig m;
    m.base.seed = seed;
    m.base.count = count;
    m.base.band = band;
    m.base.ioc_percent = 10;
    m.base.fok_percent = 8;
    m.base.market_percent = 8;
    m.base.amend_percent = 12;
    m.symbols = symbols;
    return generate_multi(m);
}

using Books = std::map<std::string, BookSnapshot>;  // keyed by NAME: log ids are log-local

template <class Engines>
Books books_of(const SymbolTable& t, const Engines& engines) {
    Books b;
    engines.for_each([&](SymbolId s, const auto& e) { b[t.name(s)] = e.book().snapshot(); });
    return b;
}

// State of an uninterrupted sequential run after each prefix of `commands` (index k = after k commands).
std::vector<Books> prefix_states(const GeneratedMulti& g) {
    std::vector<Books> out;
    MultiEngine m;
    std::vector<Event> scratch;
    out.push_back(books_of(g.symbols, m));
    for (const SymbolCommand& c : g.commands) {
        scratch.clear();
        m.apply(c.symbol, c.command, scratch);
        out.push_back(books_of(g.symbols, m));
    }
    return out;
}

// Writes every command to a fresh log; returns the file offset after each command (index k = after k commands).
std::vector<std::uint64_t> write_log(const std::string& path, const GeneratedMulti& g, const WalOptions& o = {}) {
    WalWriter w = WalWriter::create(path, o);
    std::vector<std::uint64_t> boundary{w.bytes_written()};
    for (const SymbolCommand& c : g.commands) {
        w.append(g.symbols.name(c.symbol), c.command);
        boundary.push_back(w.bytes_written());
    }
    w.close();
    return boundary;
}

struct Scanned {
    WalScan scan;
    SymbolTable symbols;
    MultiEngine engines;
    std::vector<std::tuple<std::uint64_t, std::string, Command>> seen;
};
// Scan bytes held in memory and apply what is valid (the same thing recover_wal does for a file).
Scanned scan_bytes(const std::string& bytes) {
    Scanned s;
    std::istringstream in(bytes);
    std::vector<Event> scratch;
    s.scan = wal_scan(in, [&](std::uint64_t seq, const std::string& sym, const Command& c) {
        s.seen.emplace_back(seq, sym, c);
        scratch.clear();
        s.engines.apply(s.symbols.intern(sym), c, scratch);
    });
    return s;
}

// ---- hand-built records, from the documented layout (so the docs and the code cannot drift apart) ----
void le(std::string& s, std::uint64_t v, int n) { for (int i = 0; i < n; ++i) s.push_back(static_cast<char>(v >> (8 * i))); }
std::string record(unsigned char kind, const std::string& body) {
    const std::string rest = std::string(1, static_cast<char>(kind)) + body;
    std::string out;
    le(out, wal_crc32(rest.data(), rest.size()), 4);
    return out + rest;
}
std::string sym_def(std::uint32_t id, const std::string& name) {
    std::string b;
    le(b, id, 4);
    b.push_back(static_cast<char>(name.size()));
    return record(1, b + name);
}
std::string new_rec(std::uint64_t seq, std::uint32_t sym, std::uint64_t id, std::int64_t price, std::uint64_t qty,
                    unsigned char side = 0, unsigned char tif = 0, unsigned char type = 0) {
    std::string b;
    le(b, seq, 8); le(b, sym, 4); le(b, id, 8); le(b, static_cast<std::uint64_t>(price), 8); le(b, qty, 8);
    b.push_back(static_cast<char>(side)); b.push_back(static_cast<char>(tif)); b.push_back(static_cast<char>(type));
    return record(2, b);
}
const std::string kHeader(kWalMagic, kWalHeaderBytes);

}  // namespace

// ---- CRC -------------------------------------------------------------------------------------------

TEST(WalCrc, KnownVectorsAndChaining) {
    EXPECT_EQ(wal_crc32("123456789", 9), 0xCBF43926u);
    EXPECT_EQ(wal_crc32("", 0), 0u);
    EXPECT_EQ(wal_crc32("a", 1), 0xE8B7BE43u);
    EXPECT_EQ(wal_crc32("6789", 4, wal_crc32("12345", 5)), 0xCBF43926u);
}

TEST(WalCrc, DetectsEverySingleBitFlipInARecordSizedBuffer) {
    std::string buf(44, '\0');
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<char>(i * 37 + 11);
    const std::uint32_t good = wal_crc32(buf.data(), buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) {
            buf[i] ^= static_cast<char>(1 << bit);
            EXPECT_NE(wal_crc32(buf.data(), buf.size()), good) << i << ":" << bit;
            buf[i] ^= static_cast<char>(1 << bit);
        }
}

// ---- write / scan round trip -----------------------------------------------------------------------

TEST(Wal, RoundTripReproducesEveryCommandInOrder) {
    const GeneratedMulti g = busy(1, 3000, 5);
    TempPath f("rt");
    write_log(f.str(), g);
    std::vector<std::tuple<std::uint64_t, std::string, Command>> seen;
    const WalScan s = wal_scan_file(f.str(), [&](std::uint64_t q, const std::string& n, const Command& c) { seen.emplace_back(q, n, c); });
    EXPECT_EQ(s.status, WalStatus::Clean);
    EXPECT_EQ(s.valid_bytes, s.file_bytes);
    EXPECT_EQ(s.commands, g.commands.size());
    EXPECT_EQ(s.last_seq, g.commands.size());
    ASSERT_EQ(seen.size(), g.commands.size());
    for (std::size_t i = 0; i < seen.size(); ++i) {
        ASSERT_EQ(std::get<0>(seen[i]), i + 1);
        ASSERT_EQ(std::get<1>(seen[i]), g.symbols.name(g.commands[i].symbol));
        ASSERT_EQ(std::get<2>(seen[i]), g.commands[i].command) << i;
    }
}

TEST(Wal, ExtremeFieldValuesSurviveTheRoundTrip) {
    TempPath f("ext");
    {
        WalWriter w = WalWriter::create(f.str());
        w.append("X", NewCommand{Order(~0ULL, Side::Sell, 4611686018427387903LL, ~0ULL, TimeInForce::FOK, OrderType::Limit)});
        w.append("X", NewCommand{Order(0, Side::Buy, -5, 0, TimeInForce::GTC, OrderType::Limit)});  // invalid orders are logged too
        w.append("X", NewCommand{Order::market(7, Side::Buy, 9)});
        w.append("X", AmendCommand{~0ULL, -1, ~0ULL});
        w.append("X", CancelCommand{~0ULL});
    }
    const Scanned s = scan_bytes(slurp(f.str()));
    ASSERT_EQ(s.scan.status, WalStatus::Clean);
    ASSERT_EQ(s.seen.size(), 5u);
    EXPECT_EQ(std::get<2>(s.seen[0]), Command(NewCommand{Order(~0ULL, Side::Sell, 4611686018427387903LL, ~0ULL, TimeInForce::FOK, OrderType::Limit)}));
    EXPECT_EQ(std::get<2>(s.seen[1]), Command(NewCommand{Order(0, Side::Buy, -5, 0, TimeInForce::GTC, OrderType::Limit)}));
    EXPECT_EQ(std::get<2>(s.seen[2]), Command(NewCommand{Order::market(7, Side::Buy, 9)}));
    EXPECT_EQ(std::get<2>(s.seen[3]), Command(AmendCommand{~0ULL, -1, ~0ULL}));
    EXPECT_EQ(std::get<2>(s.seen[4]), Command(CancelCommand{~0ULL}));
}

TEST(Wal, SameInputGivesIdenticalBytesAndSnapshotsAreNeverLogged) {
    const GeneratedMulti g = busy(2, 1500, 4);
    TempPath a("a"), b("b");
    write_log(a.str(), g);
    write_log(b.str(), g);
    EXPECT_EQ(slurp(a.str()), slurp(b.str()));
    TempPath c("c");
    WalWriter w = WalWriter::create(c.str());
    const auto before = w.bytes_written();
    EXPECT_THROW(w.append("A", SnapshotCommand{}), std::invalid_argument);
    EXPECT_EQ(w.bytes_written(), before);  // nothing was written
}

TEST(Wal, InvalidSymbolIsRejectedBeforeAnythingIsWritten) {
    TempPath f("sym");
    WalWriter w = WalWriter::create(f.str());
    const auto before = w.bytes_written();
    for (const char* bad : {"", "has space", "THIS-NAME-IS-TOO-LONG", "a/b"})
        EXPECT_THROW(w.append(bad, CancelCommand{1}), std::invalid_argument) << bad;
    EXPECT_EQ(w.bytes_written(), before);
    EXPECT_EQ(w.last_seq(), 0u);
    w.append("OK", CancelCommand{1});  // and the writer is still usable
    EXPECT_EQ(w.last_seq(), 1u);
}

TEST(Wal, CreateRefusesToClobberAnExistingFile) {
    TempPath f("exists");
    WalWriter::create(f.str()).close();
    EXPECT_THROW(WalWriter::create(f.str()), std::runtime_error);
    EXPECT_THROW(WalWriter::create("/nonexistent_dir_xyz/log"), std::runtime_error);
}

TEST(Wal, ManySymbolsAndTheSymbolCap) {
    TempPath f("many");
    {
        WalWriter w = WalWriter::create(f.str());
        for (std::size_t i = 0; i < kMaxSymbols; ++i) w.append("S" + std::to_string(i), CancelCommand{1});
        EXPECT_THROW(w.append("ONE-TOO-MANY", CancelCommand{1}), std::length_error);
        w.append("S0", CancelCommand{2});  // existing symbols keep working
    }
    const WalScan s = wal_scan_file(f.str(), nullptr);
    EXPECT_EQ(s.status, WalStatus::Clean);
    EXPECT_EQ(s.symbols.size(), kMaxSymbols);
    EXPECT_EQ(s.commands, kMaxSymbols + 1u);
}

// ---- recovery equals the uninterrupted run ----------------------------------------------------------

TEST(WalRecovery, RecoveredBooksEqualTheUninterruptedRunOnBothEngines) {
    for (std::uint64_t seed : {11, 12, 13}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const GeneratedMulti g = busy(seed, 8000, 6, seed == 12 ? 2 : 5);
        TempPath f("rec");
        write_log(f.str(), g);
        const PipelineResult ref = run_sequential(g.commands, PipelineOptions{.shards = 1});
        const Books want = prefix_states(g).back();
        ASSERT_GT(ref.stats.trades, 100u);

        auto a = recover_wal<StdPolicy>(f.str());
        auto b = recover_wal<PoolPolicy>(f.str());
        EXPECT_EQ(a.scan.status, WalStatus::Clean);
        EXPECT_EQ(books_of(a.symbols, a.engines), want);
        EXPECT_EQ(books_of(b.symbols, b.engines), want);
        // Replaying the log also reproduces the run's event statistics exactly.
        EXPECT_EQ(a.stats.commands, g.commands.size());
        EXPECT_EQ(a.stats.trades, ref.stats.trades);
        EXPECT_EQ(a.stats.traded_quantity, ref.stats.traded_quantity);
        EXPECT_EQ(a.stats.accepted, ref.stats.accepted);
        EXPECT_EQ(a.stats.expired, ref.stats.expired);
        EXPECT_EQ(a.stats.amended, ref.stats.amended);
    }
}

// ---- THE crash-safety property ----------------------------------------------------------------------

TEST(WalCrash, TruncationAtEveryByteRecoversExactlyAPrefixOfTheCommands) {
    const GeneratedMulti g = busy(21, 70, 3);
    TempPath f("trunc");
    const std::vector<std::uint64_t> boundary = write_log(f.str(), g);
    const std::string full = slurp(f.str());
    const std::vector<Books> prefix = prefix_states(g);
    ASSERT_EQ(boundary.back(), full.size());
    ASSERT_GT(full.size(), 2000u);

    std::size_t last_commands = 0;
    for (std::size_t t = 0; t <= full.size(); ++t) {
        const Scanned s = scan_bytes(full.substr(0, t));
        // k = commands whose last byte lies within the first t bytes.
        std::size_t k = 0;
        while (k + 1 < boundary.size() && boundary[k + 1] <= t) ++k;

        ASSERT_EQ(s.scan.commands, k) << "t=" << t;
        ASSERT_TRUE(s.scan.usable()) << "t=" << t << " " << to_string(s.scan.status);
        ASSERT_LE(s.scan.valid_bytes, t) << "t=" << t;
        ASSERT_GE(s.scan.valid_bytes, t < kWalHeaderBytes ? 0u : (k == 0 ? kWalHeaderBytes : boundary[k])) << "t=" << t;
        // Clean exactly when every byte belongs to a valid record. A file shorter than the header (including an
        // empty one) is a torn header: nothing valid, but recoverable as an empty log.
        ASSERT_EQ(s.scan.status == WalStatus::Clean, t >= kWalHeaderBytes && s.scan.valid_bytes == t) << "t=" << t;
        if (t < kWalHeaderBytes) { ASSERT_EQ(s.scan.status, WalStatus::TornTail) << "t=" << t; }
        ASSERT_EQ(books_of(s.symbols, s.engines), prefix[k]) << "state after truncation at " << t << " is not the state after " << k << " commands";
        ASSERT_GE(s.scan.commands, last_commands) << "recovery went backwards at t=" << t;  // monotone in t
        last_commands = s.scan.commands;

        // Idempotence: the valid prefix, by itself, is a clean log with the same content.
        if (t % 7 == 0) {
            const Scanned again = scan_bytes(full.substr(0, s.scan.valid_bytes));
            // (a zero-byte "prefix" is just a torn header again; anything with a full header must scan Clean)
            ASSERT_EQ(again.scan.status, s.scan.valid_bytes == 0 ? WalStatus::TornTail : WalStatus::Clean) << "t=" << t;
            ASSERT_EQ(again.scan.commands, s.scan.commands);
        }
    }
    EXPECT_EQ(last_commands, g.commands.size());
}

TEST(WalCrash, EverySingleBitFlipIsDetectedOrIsConfinedToTheFinalRecord) {
    const GeneratedMulti g = busy(22, 70, 3);
    TempPath f("flip");
    const std::vector<std::uint64_t> boundary = write_log(f.str(), g);
    const std::string full = slurp(f.str());
    const std::vector<Books> prefix = prefix_states(g);
    const std::size_t size = full.size();
    std::size_t detected_mid = 0, tail_cases = 0;

    for (std::size_t b = 0; b < size; ++b)
        for (int bit : {0, 7}) {
            std::string bad = full;
            bad[b] ^= static_cast<char>(1 << bit);
            const Scanned s = scan_bytes(bad);
            if (b < kWalHeaderBytes) {
                ASSERT_EQ(s.scan.status, WalStatus::BadHeader) << "byte " << b;
                continue;
            }
            // Commands that lie completely before the damaged byte are always recovered, nothing more.
            std::size_t k = 0;
            while (k + 1 < boundary.size() && boundary[k + 1] <= b) ++k;
            ASSERT_EQ(s.scan.commands, k) << "byte " << b << " bit " << bit;
            ASSERT_EQ(books_of(s.symbols, s.engines), prefix[k]) << "byte " << b;
            ASSERT_NE(s.scan.status, WalStatus::Clean) << "a flipped bit went undetected at byte " << b;
            if (size - b > kWalMaxRecordBytes) {
                // Far from the end: it cannot be a torn write, so it MUST be reported as corruption.
                ASSERT_EQ(s.scan.status, WalStatus::Corrupt) << "byte " << b << " bit " << bit << " silently treated as a torn tail";
                ++detected_mid;
            } else {
                ASSERT_TRUE(s.scan.status == WalStatus::Corrupt || s.scan.status == WalStatus::TornTail);
                ++tail_cases;
            }
        }
    EXPECT_GT(detected_mid, 3000u);  // the loop really covered the bulk of the file
    EXPECT_GT(tail_cases, 50u);
}

// ---- classification of damage ------------------------------------------------------------------------

TEST(WalScan, HeaderCases) {
    EXPECT_EQ(scan_bytes("").scan.status, WalStatus::TornTail);          // crashed before anything was written
    EXPECT_EQ(scan_bytes("").scan.valid_bytes, 0u);
    EXPECT_EQ(scan_bytes("LOBW").scan.status, WalStatus::TornTail);       // partial header
    EXPECT_EQ(scan_bytes(kHeader).scan.status, WalStatus::Clean);         // header only: an empty log
    EXPECT_EQ(scan_bytes(kHeader).scan.commands, 0u);
    EXPECT_EQ(scan_bytes("LOBWAL02").scan.status, WalStatus::BadHeader);  // another version
    EXPECT_EQ(scan_bytes("NEW 1 BUY 100 5\n").scan.status, WalStatus::BadHeader);  // a replay text file
}

TEST(WalScan, ChecksumFailureOnFinalRecordIsTornButMidLogIsCorrupt) {
    std::string good = kHeader + sym_def(0, "A") + new_rec(1, 0, 1, 100, 5);
    std::string bad_last = good + new_rec(2, 0, 2, 101, 6);
    bad_last.back() ^= 1;
    EXPECT_EQ(scan_bytes(bad_last).scan.status, WalStatus::TornTail);
    EXPECT_EQ(scan_bytes(bad_last).scan.commands, 1u);

    std::string bad_mid = good + new_rec(2, 0, 2, 101, 6);
    bad_mid[good.size() + 10] ^= 1;
    bad_mid += new_rec(3, 0, 3, 102, 7);  // more data after the damage
    EXPECT_EQ(scan_bytes(bad_mid).scan.status, WalStatus::Corrupt);
    EXPECT_EQ(scan_bytes(bad_mid).scan.commands, 1u);
}

TEST(WalScan, ValidChecksumButBrokenInvariantIsAlwaysCorruptEvenAtTheTail) {
    const std::string base = kHeader + sym_def(0, "A") + new_rec(1, 0, 1, 100, 5);
    struct Case { const char* what; std::string log; };
    const Case cases[] = {
        {"sequence gap", base + new_rec(3, 0, 2, 100, 5)},
        {"duplicate sequence", base + new_rec(1, 0, 2, 100, 5)},
        {"sequence going backwards", base + new_rec(2, 0, 2, 100, 5) + new_rec(1, 0, 3, 100, 5)},
        {"undefined symbol id", base + new_rec(2, 5, 2, 100, 5)},
        {"first record is not seq 1", kHeader + sym_def(0, "A") + new_rec(2, 0, 1, 100, 5)},
        {"symbol id not dense", base + sym_def(2, "B")},
        {"symbol defined twice", base + sym_def(1, "A")},
        {"invalid symbol name", base + sym_def(1, "B C")},
        {"invalid side", base + new_rec(2, 0, 2, 100, 5, /*side=*/7)},
        {"invalid time in force", base + new_rec(2, 0, 2, 100, 5, 0, /*tif=*/3)},
        {"invalid order type", base + new_rec(2, 0, 2, 100, 5, 0, 0, /*type=*/2)},
        {"command before any symbol definition", kHeader + new_rec(1, 0, 1, 100, 5)},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        EXPECT_EQ(scan_bytes(c.log).scan.status, WalStatus::Corrupt);
    }
    // Sanity: the same builder produces a log that scans clean, so the cases above fail for the stated reason.
    EXPECT_EQ(scan_bytes(base + new_rec(2, 0, 2, 100, 5) + sym_def(1, "B") + new_rec(3, 1, 1, 9, 9)).scan.status, WalStatus::Clean);
    // A duplicated final record (e.g. a replayed write) is caught by its sequence number.
    const std::string last = new_rec(2, 0, 2, 100, 5);
    EXPECT_EQ(scan_bytes(base + last + last).scan.status, WalStatus::Corrupt);
}

TEST(WalScan, UnknownKindIsCorruptUnlessItStartsInTheTailRegion) {
    const std::string base = kHeader + sym_def(0, "A") + new_rec(1, 0, 1, 100, 5);
    const std::string junk = record(9, std::string(30, 'x'));
    EXPECT_EQ(scan_bytes(base + junk).scan.status, WalStatus::TornTail);  // starts within the last 44 bytes: cannot tell
    EXPECT_EQ(scan_bytes(base + junk + new_rec(2, 0, 2, 100, 5)).scan.status, WalStatus::Corrupt);  // plenty after it
}

// ---- resuming ---------------------------------------------------------------------------------------

TEST(WalResume, CleanLogContinuesTheSequenceAndSymbols) {
    const GeneratedMulti g = busy(31, 2000, 4);
    TempPath f("resume");
    const std::size_t half = g.commands.size() / 2;
    {
        WalWriter w = WalWriter::create(f.str());
        for (std::size_t i = 0; i < half; ++i) w.append(g.symbols.name(g.commands[i].symbol), g.commands[i].command);
    }
    {
        WalScan s;
        WalWriter w = WalWriter::open_append(f.str(), {}, &s);
        EXPECT_EQ(s.status, WalStatus::Clean);
        EXPECT_EQ(w.last_seq(), half);
        for (std::size_t i = half; i < g.commands.size(); ++i) w.append(g.symbols.name(g.commands[i].symbol), g.commands[i].command);
    }
    TempPath whole("whole");
    write_log(whole.str(), g);
    EXPECT_EQ(slurp(f.str()), slurp(whole.str())) << "resuming must produce the same file as never stopping";
}

TEST(WalResume, TornTailIsTruncatedThenAppendingContinuesCleanly) {
    const GeneratedMulti g = busy(32, 500, 3);
    TempPath f("tornresume");
    const auto boundary = write_log(f.str(), g);
    const std::string full = slurp(f.str());
    const std::size_t keep = 200;                                   // commands that fully survive
    spit(f.str(), full.substr(0, boundary[keep] + 9));              // cut 9 bytes into the next record(s)
    WalScan s;
    {
        WalWriter w = WalWriter::open_append(f.str(), {}, &s);
        EXPECT_EQ(s.status, WalStatus::TornTail);
        EXPECT_EQ(w.last_seq(), keep);
        for (std::size_t i = keep; i < g.commands.size(); ++i) w.append(g.symbols.name(g.commands[i].symbol), g.commands[i].command);
    }
    EXPECT_EQ(slurp(f.str()), full) << "after repair and re-appending the lost commands the file is byte-identical";
    EXPECT_EQ(wal_scan_file(f.str(), nullptr).status, WalStatus::Clean);
}

TEST(WalResume, CorruptOrForeignFilesAreNeverRepaired) {
    TempPath f("corrupt");
    const GeneratedMulti g = busy(33, 200, 2);
    write_log(f.str(), g);
    std::string bytes = slurp(f.str());
    bytes[bytes.size() / 2] ^= 1;
    spit(f.str(), bytes);
    const std::string before = slurp(f.str());
    EXPECT_THROW(WalWriter::open_append(f.str()), std::runtime_error);
    EXPECT_EQ(slurp(f.str()), before) << "a refused resume must not modify the file";

    TempPath foreign("foreign");
    spit(foreign.str(), "this is not a log\n");
    EXPECT_THROW(WalWriter::open_append(foreign.str()), std::runtime_error);
    EXPECT_EQ(slurp(foreign.str()), "this is not a log\n");
}

TEST(WalResume, TornHeaderStartsTheFileOver) {
    TempPath f("tornheader");
    spit(f.str(), "LOB");
    WalWriter w = WalWriter::open_append(f.str());
    w.append("A", CancelCommand{1});
    w.close();
    const WalScan s = wal_scan_file(f.str(), nullptr);
    EXPECT_EQ(s.status, WalStatus::Clean);
    EXPECT_EQ(s.commands, 1u);
}

// ---- the write-ahead engine -------------------------------------------------------------------------

TEST(Durable, ApplyThenCrashThenReopenReproducesTheBooks) {
    const GeneratedMulti g = busy(41, 4000, 5);
    TempPath f("durable");
    Books before;
    {
        auto e = DurableEngine::open(f.str());
        std::vector<Event> out;
        for (const SymbolCommand& c : g.commands) e.apply(g.symbols.name(c.symbol), c.command, out);
        before = books_of(e.symbols(), e.engines());
        // no close(): the destructor runs, like a process that simply exits
    }
    WalScan s;
    auto again = DurableEngine::open(f.str(), {}, &s);
    EXPECT_EQ(s.status, WalStatus::Clean);
    EXPECT_EQ(books_of(again.symbols(), again.engines()), before);
    EXPECT_EQ(books_of(again.symbols(), again.engines()), prefix_states(g).back());

    // And it keeps going: apply more, reopen once more, compare with one uninterrupted run.
    std::vector<Event> out;
    again.apply("EXTRA", NewCommand{Order(1, Side::Buy, 50, 5)}, out);
    again.close();
    auto third = DurableEngine::open(f.str());
    EXPECT_EQ(third.symbols().size(), 6u);
    EXPECT_NE(third.engines().find(5), nullptr);
}

TEST(Durable, SnapshotIsNeitherLoggedNorAppliedAndBadSymbolChangesNothing) {
    TempPath f("dur2");
    auto e = DurableEngine::open(f.str());
    std::vector<Event> out;
    e.apply("A", NewCommand{Order(1, Side::Buy, 10, 1)}, out);
    const auto bytes = e.wal().bytes_written();
    e.apply("A", SnapshotCommand{}, out);
    EXPECT_EQ(e.wal().bytes_written(), bytes);
    EXPECT_THROW(e.apply("bad name", NewCommand{Order(2, Side::Buy, 10, 1)}, out), std::invalid_argument);
    EXPECT_EQ(e.wal().bytes_written(), bytes);
    EXPECT_EQ(e.symbols().size(), 1u);
    EXPECT_EQ(e.engines().find(0)->book().order_count(), 1u);
}

// Write-ahead: if the log write fails, the command must NOT have been applied. /dev/full makes every flush fail.
TEST(Durable, FailedLogWriteMeansTheCommandWasNotApplied) {
    if (!fs::exists("/dev/full")) GTEST_SKIP() << "/dev/full not available";
    std::FILE* f = std::fopen("/dev/full", "wb");
    ASSERT_NE(f, nullptr);
    WalOptions o;
    o.sync_every = 1;  // flush after every record, which is where ENOSPC surfaces
    DurableEngine e(WalWriter::from_file(f, o), SymbolTable{}, MultiEngine{});
    std::vector<Event> out;
    EXPECT_THROW(e.apply("A", NewCommand{Order(1, Side::Buy, 10, 1)}, out), std::runtime_error);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(e.engines().find(0), nullptr) << "the command was applied although the log write failed";
    // The writer is poisoned: nothing more can follow a possibly half-written record. Observable as: the
    // rejected append must not touch the writer's bookkeeping at all (a non-poisoned writer would have
    // written the record into its buffer, and counted it, before the flush failed again).
    const auto bytes_before = e.wal().bytes_written();
    const auto seq_before = e.wal().last_seq();
    EXPECT_THROW(e.apply("A", NewCommand{Order(2, Side::Buy, 10, 1)}, out), std::runtime_error);
    EXPECT_EQ(e.wal().bytes_written(), bytes_before);
    EXPECT_EQ(e.wal().last_seq(), seq_before);
    EXPECT_THROW(e.sync(), std::runtime_error);
}

TEST(Durable, SyncEveryNAndSyncEveryRecordProduceTheSameBytes) {
    const GeneratedMulti g = busy(42, 600, 3);
    TempPath a("s0"), b("s1"), c("s50");
    write_log(a.str(), g, WalOptions{0});
    write_log(b.str(), g, WalOptions{1});
    write_log(c.str(), g, WalOptions{50});
    EXPECT_EQ(slurp(a.str()), slurp(b.str()));
    EXPECT_EQ(slurp(a.str()), slurp(c.str()));
}

// ---- journal hook: pipeline and replay -----------------------------------------------------------------

TEST(Journal, PipelineJournalRunsOnTheCallerInInputOrderBeforeDispatchAndSkipsSnapshots) {
    const auto p = parse_multi_commands("@A NEW 1 BUY 100 5\n@A SNAPSHOT\n@B NEW 1 SELL 90 3\n@B SNAPSHOT 2\n@A CANCEL 1\n");
    ASSERT_TRUE(p.ok());
    std::vector<SymbolCommand> seen;
    std::vector<std::thread::id> threads;
    PipelineOptions po;
    po.shards = 3;
    po.journal = [&](const SymbolCommand& c) { seen.push_back(c); threads.push_back(std::this_thread::get_id()); };
    const PipelineResult r = run_pipeline(p.commands, po);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], p.commands[0]);
    EXPECT_EQ(seen[1], p.commands[2]);
    EXPECT_EQ(seen[2], p.commands[4]);
    for (const auto& t : threads) EXPECT_EQ(t, std::this_thread::get_id());
    EXPECT_EQ(r.stats.commands, 3u);
}

TEST(Journal, ThrowingJournalSubmitsNothingAndPropagates) {
    BasicShardedPipeline<StdPolicy> pipe(PipelineOptions{.shards = 2, .journal = [n = 0](const SymbolCommand&) mutable { if (++n == 3) throw std::runtime_error("disk full"); }});
    pipe.submit(SymbolCommand{0, NewCommand{Order(1, Side::Buy, 10, 1)}});
    pipe.submit(SymbolCommand{1, NewCommand{Order(1, Side::Buy, 10, 1)}});
    EXPECT_THROW(pipe.submit(SymbolCommand{0, NewCommand{Order(2, Side::Buy, 11, 1)}}), std::runtime_error);
    const PipelineResult r = pipe.finish();
    EXPECT_EQ(r.stats.commands, 2u) << "the command whose journal write failed must not have been dispatched";
    EXPECT_EQ(r.stats.accepted, 2u);
}

TEST(Journal, ThreadedAndSequentialRunsWriteIdenticalLogsAndRecoveryMatchesThePipeline) {
    const GeneratedMulti g = busy(51, 6000, 6);
    TempPath seq("jseq"), thr("jthr");
    for (const auto& [path, threads] : {std::pair{seq.str(), std::size_t{0}}, std::pair{thr.str(), std::size_t{4}}}) {
        WalWriter w = WalWriter::create(path);
        MultiReplayOptions o;
        o.print_events = false;
        o.final_snapshot = false;
        o.threads = threads;
        o.journal = [&](const SymbolCommand& c) { w.append(g.symbols.name(c.symbol), c.command); };
        std::ostringstream sink;
        multi_replay(g.symbols, g.commands, sink, o);
        w.close();
    }
    EXPECT_EQ(slurp(seq.str()), slurp(thr.str())) << "the sequencer, not thread timing, defines the log";

    const PipelineResult pr = run_pipeline(g.commands, PipelineOptions{.shards = 4});
    auto rec = recover_wal<StdPolicy>(thr.str());
    Books from_pipeline;
    for (const SymbolFinal& f : pr.finals) from_pipeline[g.symbols.name(f.symbol)] = f.book;
    EXPECT_EQ(books_of(rec.symbols, rec.engines), from_pipeline);
}

// ---- robustness: the scanner parses untrusted bytes ---------------------------------------------------

// A seeded mini-fuzzer. No assertion about WHAT is found, only that the scanner always returns, never reads
// out of bounds (this test is run under ASan/UBSan in CI) and never contradicts itself.
TEST(WalFuzz, ArbitraryDamageNeverCrashesAndTheScanStaysSelfConsistent) {
    const GeneratedMulti g = busy(61, 120, 4);
    TempPath f("fuzz");
    write_log(f.str(), g);
    const std::string good = slurp(f.str());
    std::mt19937_64 rng(12345);
    auto below = [&](std::uint64_t n) { return rng() % n; };

    std::size_t statuses[4] = {0, 0, 0, 0};
    for (int iter = 0; iter < 4000; ++iter) {
        std::string bytes = good;
        switch (iter % 5) {
            case 0:  // random byte changes
                for (std::uint64_t k = 1 + below(4); k > 0; --k) bytes[below(bytes.size())] = static_cast<char>(below(256));
                break;
            case 1:  // random truncation plus a change
                bytes.resize(below(bytes.size() + 1));
                if (!bytes.empty()) bytes[below(bytes.size())] ^= static_cast<char>(1 + below(255));
                break;
            case 2:  // insertion of junk
                bytes.insert(below(bytes.size() + 1), std::string(1 + below(60), static_cast<char>(below(256))));
                break;
            case 3:  // a deletion
                if (bytes.size() > 20) bytes.erase(below(bytes.size() - 10), 1 + below(10));
                break;
            default:  // valid header followed by pure garbage
                bytes = kHeader;
                for (std::uint64_t k = below(200); k > 0; --k) bytes.push_back(static_cast<char>(below(256)));
                break;
        }
        std::size_t visited = 0;
        std::istringstream in(bytes);
        const WalScan s = wal_scan(in, [&](std::uint64_t seq, const std::string& sym, const Command&) {
            ++visited;
            ASSERT_EQ(seq, visited);
            ASSERT_TRUE(SymbolTable::valid_name(sym));
        });
        ASSERT_EQ(s.file_bytes, bytes.size());
        ASSERT_LE(s.valid_bytes, s.file_bytes);
        ASSERT_EQ(s.commands, visited);
        ASSERT_EQ(s.last_seq, visited);
        if (s.status == WalStatus::Clean) { ASSERT_EQ(s.valid_bytes, s.file_bytes); }
        if (s.status != WalStatus::BadHeader && s.status != WalStatus::Clean) { ASSERT_FALSE(s.message.empty()); }
        ++statuses[static_cast<int>(s.status)];
        // Whatever the damage, a usable valid prefix is a CLEAN log on its own (or an empty/torn header).
        if (s.usable() && s.valid_bytes >= kWalHeaderBytes) {
            const WalScan again = scan_bytes(bytes.substr(0, s.valid_bytes)).scan;
            ASSERT_EQ(again.status, WalStatus::Clean) << "iter " << iter;
            ASSERT_EQ(again.commands, s.commands);
        }
    }
    // The campaign really exercised every outcome, so it is not vacuous.
    EXPECT_GT(statuses[static_cast<int>(WalStatus::Clean)], 0u);
    EXPECT_GT(statuses[static_cast<int>(WalStatus::TornTail)], 100u);
    EXPECT_GT(statuses[static_cast<int>(WalStatus::Corrupt)], 100u);
    EXPECT_GT(statuses[static_cast<int>(WalStatus::BadHeader)], 0u);
}
