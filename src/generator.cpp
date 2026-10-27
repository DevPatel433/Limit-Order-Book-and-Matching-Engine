#include "lob/generator.hpp"

#include <algorithm>
#include <random>

namespace lob {

namespace {
struct Rng {
    std::mt19937_64 gen;
    explicit Rng(std::uint64_t seed) : gen(seed) {}
    std::uint64_t below(std::uint64_t n) { return gen() % n; }  // n > 0
};
}  // namespace

std::vector<Command> generate(const GeneratorConfig& c) {
    Rng rng(c.seed);
    std::vector<Command> out;
    out.reserve(c.count);
    OrderId next_id = 1;
    std::vector<Price> price_of(1, 0);  // price each issued ID was submitted at (index = ID); no RNG use
    const Price band = std::max<Price>(0, std::min<Price>(c.band, c.mid - 1));
    const Quantity max_q = std::max<Quantity>(1, c.max_quantity);

    for (std::size_t i = 0; i < c.count; ++i) {
        if (c.snapshot_every != 0 && (i + 1) % c.snapshot_every == 0) {
            out.emplace_back(SnapshotCommand{});
            continue;
        }

        if (next_id > 1 && c.amend_percent != 0 && rng.below(100) < c.amend_percent) {
            // Mostly recent IDs (likely still resting); sometimes any past or never-issued ID.
            AmendCommand a;
            const std::uint64_t window = std::min<std::uint64_t>(next_id - 1, 40);
            a.id = rng.below(100) < 85 ? next_id - 1 - rng.below(window) : 1 + rng.below(next_id + 3);
            // Half the time keep the original price, so quantity-only amends (which may keep time
            // priority) are common; otherwise pick a fresh price in the band (loses priority, may cross).
            if (a.id < price_of.size() && rng.below(2) == 0) a.price = price_of[a.id];
            else a.price = c.mid - band + static_cast<Price>(rng.below(static_cast<std::uint64_t>(2 * band + 1)));
            a.quantity = 1 + rng.below(max_q);
            out.emplace_back(a);
            continue;
        }

        if (next_id > 1 && rng.below(100) < c.cancel_percent) {
            // Mostly past IDs (live or dead); sometimes a never-issued one.
            out.emplace_back(CancelCommand{1 + rng.below(next_id + 3)});
            continue;
        }

        Order o;
        o.side = rng.below(2) ? Side::Buy : Side::Sell;
        o.price = c.mid - band + static_cast<Price>(rng.below(static_cast<std::uint64_t>(2 * band + 1)));
        o.quantity = 1 + rng.below(max_q);
        if (rng.below(100) < c.sweep_percent) o.quantity *= 10;
        o.id = next_id;
        if (c.ioc_percent != 0 && rng.below(100) < c.ioc_percent) o.tif = TimeInForce::IOC;
        else if (c.fok_percent != 0 && rng.below(100) < c.fok_percent) o.tif = TimeInForce::FOK;
        if (c.market_percent != 0 && rng.below(100) < c.market_percent) {
            o.type = OrderType::Market;
            o.price = 0;
            if (o.tif == TimeInForce::GTC) o.tif = TimeInForce::IOC;
        }

        if (rng.below(100) < c.bad_percent) {
            switch (rng.below(4)) {
                case 0: o.quantity = 0; break;                              // InvalidQuantity
                case 1: o.price = 0; break;                                 // InvalidPrice
                case 2: o.price = -static_cast<Price>(1 + rng.below(100)); break;
                default:
                    if (next_id > 1) o.id = 1 + rng.below(next_id - 1);     // maybe a duplicate
                    break;
            }
        }
        if (o.type == OrderType::Market) o.price = 0;  // canonical form: a market order's price is ignored (and prints as MKT)
        if (o.id == next_id) {
            ++next_id;
            price_of.push_back(o.price);
        }
        out.emplace_back(NewCommand{o});
    }
    return out;
}

}  // namespace lob
