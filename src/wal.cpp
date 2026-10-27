#include "lob/wal.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <set>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace lob {

namespace {

enum Kind : unsigned char { kSymbolDef = 1, kNew = 2, kCancel = 3, kAmend = 4 };

// Bytes after the kind byte, for the fixed-size kinds. (SymbolDef is 5 + name length.)
constexpr std::size_t kNewBody = 8 + 4 + 8 + 8 + 8 + 3;  // seq, symbol, id, price, qty, side/tif/type
constexpr std::size_t kCancelBody = 8 + 4 + 8;
constexpr std::size_t kAmendBody = 8 + 4 + 8 + 8 + 8;
static_assert(4 + 1 + kNewBody == kWalMaxRecordBytes, "kWalMaxRecordBytes must equal the largest record");

void put_u32(unsigned char* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
void put_u64(unsigned char* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
std::uint32_t get_u32(const unsigned char* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
std::uint64_t get_u64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

std::uint32_t wal_crc32(const void* data, std::size_t size, std::uint32_t crc) noexcept {
    const auto& t = crc_table();
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t c = ~crc;
    for (std::size_t i = 0; i < size; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

const char* to_string(WalStatus s) noexcept {
    switch (s) {
        case WalStatus::Clean: return "Clean";
        case WalStatus::TornTail: return "TornTail";
        case WalStatus::Corrupt: return "Corrupt";
        case WalStatus::BadHeader: return "BadHeader";
    }
    return "?";
}

// ---- scanner ---------------------------------------------------------------------------------------

WalScan wal_scan(std::istream& in, const WalVisitor& visit) {
    WalScan r;
    in.seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    r.file_bytes = size;

    auto finish = [&](WalStatus s, std::uint64_t valid, std::string msg) {
        r.status = s;
        r.valid_bytes = valid;
        r.message = std::move(msg);
        return r;
    };

    if (size < kWalHeaderBytes) return finish(WalStatus::TornTail, 0, "incomplete header (" + std::to_string(size) + " bytes)");
    char hdr[kWalHeaderBytes];
    in.read(hdr, kWalHeaderBytes);
    if (std::memcmp(hdr, kWalMagic, kWalHeaderBytes) != 0) return finish(WalStatus::BadHeader, 0, "not a LOBWAL01 file");

    std::uint64_t pos = kWalHeaderBytes;
    std::set<std::string> defined;
    std::uint64_t prev_seq = 0;

    for (;;) {
        const std::uint64_t start = pos;
        const std::uint64_t remaining = size - start;
        if (remaining == 0) return finish(WalStatus::Clean, start, "");
        // A record that starts this close to the end cannot be told apart from a torn write.
        const bool in_tail = remaining < kWalMaxRecordBytes;

        const auto torn = [&](const std::string& why) { return finish(WalStatus::TornTail, start, why + " at offset " + std::to_string(start)); };
        const auto corrupt = [&](const std::string& why) { return finish(WalStatus::Corrupt, start, why + " at offset " + std::to_string(start)); };
        const auto ambiguous = [&](const std::string& why) { return in_tail ? torn(why) : corrupt(why); };

        if (remaining < 5) return torn("incomplete record header");
        unsigned char head[5];
        in.read(reinterpret_cast<char*>(head), 5);
        const std::uint32_t stored_crc = get_u32(head);
        const unsigned char kind = head[4];

        std::vector<unsigned char> rec;  // kind + body: exactly what the CRC covers
        rec.push_back(kind);
        std::size_t body = 0;
        if (kind == kNew) body = kNewBody;
        else if (kind == kCancel) body = kCancelBody;
        else if (kind == kAmend) body = kAmendBody;
        else if (kind == kSymbolDef) {
            if (remaining < 5 + 5) return torn("incomplete symbol record");
            unsigned char pre[5];
            in.read(reinterpret_cast<char*>(pre), 5);
            rec.insert(rec.end(), pre, pre + 5);
            const std::size_t len = pre[4];
            if (len == 0 || len > kMaxSymbolNameLength) return ambiguous("invalid symbol name length " + std::to_string(len));
            body = 5 + len;
        } else {
            return ambiguous("unknown record kind " + std::to_string(kind));
        }
        const std::size_t have = rec.size() - 1;  // body bytes already read
        if (remaining < 5 + body) return torn("record cut short");
        rec.resize(1 + body);
        in.read(reinterpret_cast<char*>(rec.data() + 1 + have), static_cast<std::streamsize>(body - have));
        const std::uint64_t rec_end = start + 4 + rec.size();

        if (wal_crc32(rec.data(), rec.size()) != stored_crc) {
            if (rec_end == size) return torn("checksum mismatch in the final record");
            return corrupt("checksum mismatch with more data after it");
        }

        // From here the checksum is good, so any problem is a broken invariant, never a torn write.
        const unsigned char* b = rec.data() + 1;
        if (kind == kSymbolDef) {
            const std::uint32_t id = get_u32(b);
            const std::string name(reinterpret_cast<const char*>(b + 5), rec.size() - 6);
            if (id != r.symbols.size()) return corrupt("symbol id " + std::to_string(id) + " is not the next dense id");
            if (!SymbolTable::valid_name(name)) return corrupt("invalid symbol name");
            if (!defined.insert(name).second) return corrupt("symbol '" + name + "' defined twice");
            r.symbols.push_back(name);
        } else {
            const std::uint64_t seq = get_u64(b);
            const std::uint32_t sym = get_u32(b + 8);
            if (seq != prev_seq + 1) return corrupt("sequence " + std::to_string(seq) + " after " + std::to_string(prev_seq));
            if (sym >= r.symbols.size()) return corrupt("command refers to undefined symbol id " + std::to_string(sym));
            Command cmd;
            if (kind == kNew) {
                const unsigned char side = b[12 + 24], tif = b[12 + 25], type = b[12 + 26];
                if (side > 1 || tif > 2 || type > 1) return corrupt("invalid side/time-in-force/type value");
                cmd = NewCommand{Order(get_u64(b + 12), side == 0 ? Side::Buy : Side::Sell,
                                       static_cast<Price>(get_u64(b + 20)), get_u64(b + 28),
                                       static_cast<TimeInForce>(tif), static_cast<OrderType>(type))};
            } else if (kind == kCancel) {
                cmd = CancelCommand{get_u64(b + 12)};
            } else {
                cmd = AmendCommand{get_u64(b + 12), static_cast<Price>(get_u64(b + 20)), get_u64(b + 28)};
            }
            prev_seq = seq;
            ++r.commands;
            r.last_seq = seq;
            if (visit) visit(seq, r.symbols[sym], cmd);
        }
        pos = rec_end;
    }
}

WalScan wal_scan_file(const std::string& path, const WalVisitor& visit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return wal_scan(in, visit);
}

// ---- writer ----------------------------------------------------------------------------------------

void WalWriter::FileCloser::operator()(std::FILE* f) const noexcept {
    if (f) std::fclose(f);
}

WalWriter::~WalWriter() {
    if (file_) {
        std::fflush(file_.get());  // best effort; errors are only reportable through close()/sync()
    }
}

WalWriter WalWriter::create(const std::string& path, const WalOptions& options) {
    std::FILE* f = std::fopen(path.c_str(), "wbx");  // "x": fail if it exists
    if (!f) throw std::runtime_error("cannot create " + path + " (it may already exist)");
    WalWriter w;
    w.file_.reset(f);
    w.options_ = options;
    w.write_record(reinterpret_cast<const unsigned char*>(kWalMagic), kWalHeaderBytes);
    w.bytes_ = kWalHeaderBytes;
    if (std::fflush(f) != 0) throw std::runtime_error("cannot write log header");
    return w;
}

WalWriter WalWriter::from_file(std::FILE* file, const WalOptions& options, std::uint64_t last_seq) {
    if (!file) throw std::invalid_argument("null FILE*");
    WalWriter w;
    w.file_.reset(file);
    w.options_ = options;
    w.last_seq_ = last_seq;
    return w;
}

WalWriter WalWriter::open_append(const std::string& path, const WalOptions& options, WalScan* scan_out) {
    const WalScan scan = wal_scan_file(path, nullptr);
    if (scan_out) *scan_out = scan;
    if (!scan.usable()) throw std::runtime_error("cannot resume log: " + std::string(to_string(scan.status)) + ": " + scan.message);

    if (scan.valid_bytes == 0) {  // crashed before the header was complete: start the file over
        std::filesystem::remove(path);
        WalWriter w = create(path, options);
        return w;
    }
    if (scan.status == WalStatus::TornTail) std::filesystem::resize_file(path, scan.valid_bytes);
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) throw std::runtime_error("cannot open " + path + " for appending");
    WalWriter w;
    w.file_.reset(f);
    w.options_ = options;
    w.last_seq_ = scan.last_seq;
    w.bytes_ = scan.valid_bytes;
    for (std::size_t i = 0; i < scan.symbols.size(); ++i) w.ids_.emplace(scan.symbols[i], static_cast<SymbolId>(i));
    return w;
}

void WalWriter::write_record(const unsigned char* data, std::size_t size) {
    if (poisoned_) throw std::runtime_error("log writer is unusable after an earlier I/O error");
    if (!file_) throw std::runtime_error("log writer is closed");
    if (std::fwrite(data, 1, size, file_.get()) != size) {
        poisoned_ = true;
        throw std::runtime_error("write to log failed");
    }
}

void WalWriter::note_synced_or_sync() {
    if (options_.sync_every != 0 && ++since_sync_ >= options_.sync_every) sync();
}

SymbolId WalWriter::append(std::string_view symbol, const Command& command) {
    if (std::holds_alternative<SnapshotCommand>(command)) throw std::invalid_argument("SNAPSHOT is a view and is never logged");
    if (!SymbolTable::valid_name(symbol)) throw std::invalid_argument("invalid symbol name '" + std::string(symbol) + "'");

    auto it = ids_.find(symbol);
    if (it == ids_.end() && ids_.size() >= kMaxSymbols) throw std::length_error("too many symbols in one log");

    std::array<unsigned char, 64> buf{};
    const auto emit = [&](std::size_t len /* bytes after the crc field */) {
        put_u32(buf.data(), wal_crc32(buf.data() + 4, len));
        write_record(buf.data(), 4 + len);
        bytes_ += 4 + len;
    };

    if (it == ids_.end()) {  // define the symbol first; the definition stays valid even if the command write then fails
        const auto id = static_cast<SymbolId>(ids_.size());
        buf[4] = kSymbolDef;
        put_u32(buf.data() + 5, id);
        buf[9] = static_cast<unsigned char>(symbol.size());
        std::memcpy(buf.data() + 10, symbol.data(), symbol.size());
        emit(1 + 4 + 1 + symbol.size());
        it = ids_.emplace(std::string(symbol), id).first;
    }
    const SymbolId sid = it->second;
    const std::uint64_t seq = last_seq_ + 1;

    unsigned char* b = buf.data() + 5;
    put_u64(b, seq);
    put_u32(b + 8, sid);
    if (const auto* n = std::get_if<NewCommand>(&command)) {
        const Order& o = n->order;
        buf[4] = kNew;
        put_u64(b + 12, o.id);
        put_u64(b + 20, static_cast<std::uint64_t>(o.price));
        put_u64(b + 28, o.quantity);
        b[36] = o.side == Side::Buy ? 0 : 1;
        b[37] = static_cast<unsigned char>(o.tif);
        b[38] = static_cast<unsigned char>(o.type);
        emit(1 + kNewBody);
    } else if (const auto* c = std::get_if<CancelCommand>(&command)) {
        buf[4] = kCancel;
        put_u64(b + 12, c->id);
        emit(1 + kCancelBody);
    } else {
        const auto& a = std::get<AmendCommand>(command);
        buf[4] = kAmend;
        put_u64(b + 12, a.id);
        put_u64(b + 20, static_cast<std::uint64_t>(a.price));
        put_u64(b + 28, a.quantity);
        emit(1 + kAmendBody);
    }
    last_seq_ = seq;
    note_synced_or_sync();
    return sid;
}

void WalWriter::sync() {
    if (poisoned_) throw std::runtime_error("log writer is unusable after an earlier I/O error");
    if (!file_) return;
    since_sync_ = 0;
    if (std::fflush(file_.get()) != 0) {
        poisoned_ = true;
        throw std::runtime_error("flush of log failed");
    }
#if defined(__unix__) || defined(__APPLE__)
    if (::fsync(::fileno(file_.get())) != 0) {
        poisoned_ = true;
        throw std::runtime_error("fsync of log failed");
    }
#endif
}

void WalWriter::close() {
    if (!file_) return;
    const bool ok = !poisoned_ && std::fflush(file_.get()) == 0;
    file_.reset();
    if (!ok) throw std::runtime_error("log was not flushed cleanly on close");
}

// ---- recovery --------------------------------------------------------------------------------------

template <class Policy>
BasicRecovered<Policy> recover_wal(const std::string& path) {
    BasicRecovered<Policy> out;
    std::vector<Event> scratch;
    out.scan = wal_scan_file(path, [&](std::uint64_t, const std::string& symbol, const Command& cmd) {
        const SymbolId id = out.symbols.intern(symbol);
        scratch.clear();
        out.engines.apply(id, cmd, scratch);
        ++out.stats.commands;
        for (const Event& e : scratch) tally_event(e, out.stats);
    });
    return out;
}

template BasicRecovered<StdPolicy> recover_wal<StdPolicy>(const std::string&);
template BasicRecovered<PoolPolicy> recover_wal<PoolPolicy>(const std::string&);

}  // namespace lob
