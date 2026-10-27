#include "lob/events.hpp"

#include <cassert>
#include <ostream>
#include <sstream>

namespace lob {

const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::DuplicateId: return "DuplicateId";
        case RejectReason::InvalidPrice: return "InvalidPrice";
        case RejectReason::InvalidQuantity: return "InvalidQuantity";
        case RejectReason::QuantityOverflow: return "QuantityOverflow";
        case RejectReason::UnknownOrder: return "UnknownOrder";
        case RejectReason::InvalidTimeInForce: return "InvalidTimeInForce";
    }
    return "Unknown";
}

RejectReason to_reject_reason(AddStatus s) noexcept {
    switch (s) {
        case AddStatus::DuplicateId: return RejectReason::DuplicateId;
        case AddStatus::InvalidPrice: return RejectReason::InvalidPrice;
        case AddStatus::InvalidQuantity: return RejectReason::InvalidQuantity;
        case AddStatus::QuantityOverflow: return RejectReason::QuantityOverflow;
        case AddStatus::InvalidTimeInForce: return RejectReason::InvalidTimeInForce;
        case AddStatus::Ok: break;
    }
    assert(false && "to_reject_reason called with Ok");
    return RejectReason::InvalidQuantity;
}

std::ostream& operator<<(std::ostream& os, const Accepted& e) {
    os << "ACCEPTED id=" << e.id << ' ' << to_string(e.side) << ' ';
    if (e.type == OrderType::Market) os << "MKT";
    else os << e.price;
    os << " x " << e.quantity;
    // Plain GTC limit orders keep the original, shorter format.
    if (e.tif != TimeInForce::GTC || e.type == OrderType::Market) os << ' ' << to_string(e.tif);
    return os;
}
std::ostream& operator<<(std::ostream& os, const Rejected& e) {
    return os << "REJECTED id=" << e.id << ' ' << to_string(e.reason);
}
std::ostream& operator<<(std::ostream& os, const Trade& e) {
    return os << "TRADE maker=" << e.maker_id << " taker=" << e.taker_id << ' '
              << to_string(e.taker_side) << ' ' << e.price << " x " << e.quantity;
}
std::ostream& operator<<(std::ostream& os, const Cancelled& e) {
    return os << "CANCELLED id=" << e.id << " remaining=" << e.remaining;
}

std::ostream& operator<<(std::ostream& os, const Expired& e) {
    return os << "EXPIRED id=" << e.id << " remaining=" << e.remaining;
}
std::ostream& operator<<(std::ostream& os, const Amended& e) {
    return os << "AMENDED id=" << e.id << ' ' << e.old_price << " x " << e.old_quantity << " -> " << e.new_price
              << " x " << e.new_quantity << (e.keeps_priority ? " priority=kept" : " priority=lost");
}

std::string to_string(const Event& e) {
    std::ostringstream os;
    std::visit([&os](const auto& ev) { os << ev; }, e);
    return os.str();
}

const char* csv_header() noexcept {
    return "seq,cmd,event,id,side,price,quantity,maker_id,taker_id,remaining,detail";
}

std::string to_csv_row(std::size_t seq, std::size_t cmd, const Event& ev) {
    std::ostringstream os;
    os << seq << ',' << cmd << ',';
    // columns after cmd: event,id,side,price,quantity,maker_id,taker_id,remaining,detail
    if (const auto* a = std::get_if<Accepted>(&ev)) {
        os << "ACCEPTED," << a->id << ',' << to_string(a->side) << ',';
        if (a->type == OrderType::Market) os << "MKT";
        else os << a->price;
        os << ',' << a->quantity << ",,,," << to_string(a->tif);
    } else if (const auto* t = std::get_if<Trade>(&ev)) {
        os << "TRADE," << t->taker_id << ',' << to_string(t->taker_side) << ',' << t->price << ',' << t->quantity << ','
           << t->maker_id << ',' << t->taker_id << ",,";
    } else if (const auto* c = std::get_if<Cancelled>(&ev)) {
        os << "CANCELLED," << c->id << ",,,,,," << c->remaining << ',';
    } else if (const auto* x = std::get_if<Expired>(&ev)) {
        os << "EXPIRED," << x->id << ",,,,,," << x->remaining << ',';
    } else if (const auto* m = std::get_if<Amended>(&ev)) {
        os << "AMENDED," << m->id << ",," << m->new_price << ',' << m->new_quantity << ",,,," << m->old_price << ':'
           << m->old_quantity << (m->keeps_priority ? ":kept" : ":lost");
    } else {
        const auto& r = std::get<Rejected>(ev);
        os << "REJECTED," << r.id << ",,,,,,," << to_string(r.reason);
    }
    return os.str();
}

}  // namespace lob
