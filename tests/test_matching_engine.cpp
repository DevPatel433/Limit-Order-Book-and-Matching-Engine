#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;
using Events = std::vector<Event>;

// Every engine test runs against both the baseline and the pooled engine.
template <class E>
class EngineTyped : public ::testing::Test {};
using BothEngines = ::testing::Types<MatchingEngine, PooledMatchingEngine>;
TYPED_TEST_SUITE(EngineTyped, BothEngines);

namespace {

Order buy(OrderId id, Price p, Quantity q) { return Order{id, Side::Buy, p, q}; }
Order sell(OrderId id, Price p, Quantity q) { return Order{id, Side::Sell, p, q}; }

Accepted acc(const Order& o) { return Accepted{o.id, o.side, o.price, o.quantity}; }

// Invariants that must hold after every engine call.
template <class E>
void ExpectHealthy(const E& e) {
    std::string why;
    EXPECT_TRUE(e.book().validate(&why)) << why;
    EXPECT_FALSE(e.book().is_crossed());
    if (e.book().best_bid() && e.book().best_ask()) {
        EXPECT_LT(*e.book().best_bid(), *e.book().best_ask());
    }
}

Quantity traded(const Events& evs) {
    Quantity q = 0;
    for (const Event& e : evs)
        if (const auto* t = std::get_if<Trade>(&e)) q += t->quantity;
    return q;
}

}  // namespace

// --------------------------------------------------------------- no crossing --

TYPED_TEST(EngineTyped, NonCrossingOrdersRestAndAreAccepted) {
    TypeParam e;
    EXPECT_EQ(e.submit(buy(1, 99, 10)), (Events{acc(buy(1, 99, 10))}));
    EXPECT_EQ(e.submit(sell(2, 101, 5)), (Events{acc(sell(2, 101, 5))}));
    EXPECT_EQ(e.book().best_bid(), 99);
    EXPECT_EQ(e.book().best_ask(), 101);
    EXPECT_EQ(e.book().spread(), 2);
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, LimitPriceIsRespected_NoTradeBeyondLimit) {
    TypeParam e;
    e.submit(sell(1, 102, 10));
    const Events ev = e.submit(buy(2, 101, 10));  // one tick short of the ask
    EXPECT_EQ(ev, (Events{acc(buy(2, 101, 10))}));
    EXPECT_EQ(e.book().best_bid(), 101);
    EXPECT_EQ(e.book().best_ask(), 102);
    ExpectHealthy(e);
}

// ------------------------------------------------------------------ crossing --

TYPED_TEST(EngineTyped, ExactFullFillClearsBothOrders) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    const Events ev = e.submit(buy(2, 100, 10));
    EXPECT_EQ(ev, (Events{acc(buy(2, 100, 10)), Trade{1, 2, Side::Buy, 100, 10}}));
    EXPECT_EQ(e.book().order_count(), 0u);
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, TradesExecuteAtRestingPrice_BuyerGetsPriceImprovement) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    const Events ev = e.submit(buy(2, 105, 5));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(std::get<Trade>(ev[1]).price, 100);  // maker's price, not the 105 limit
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, TradesExecuteAtRestingPrice_SellerGetsPriceImprovement) {
    TypeParam e;
    e.submit(buy(1, 100, 5));
    const Events ev = e.submit(sell(2, 95, 5));
    EXPECT_EQ(ev, (Events{acc(sell(2, 95, 5)), Trade{1, 2, Side::Sell, 100, 5}}));
    ExpectHealthy(e);
}

// ------------------------------------------------------------- partial fills --

TYPED_TEST(EngineTyped, PartiallyFilledTakerRestsRemainderAtItsLimit) {
    TypeParam e;
    e.submit(sell(1, 100, 4));
    const Events ev = e.submit(buy(2, 102, 10));
    EXPECT_EQ(ev, (Events{acc(buy(2, 102, 10)), Trade{1, 2, Side::Buy, 100, 4}}));
    EXPECT_EQ(e.book().find(2), (Order{2, Side::Buy, 102, 6}));  // residual, at limit price
    EXPECT_EQ(e.book().best_bid(), 102);
    EXPECT_FALSE(e.book().best_ask());
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, PartiallyFilledMakerKeepsQueuePosition) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    e.submit(sell(2, 100, 10));
    EXPECT_EQ(e.submit(buy(3, 100, 4)),
              (Events{acc(buy(3, 100, 4)), Trade{1, 3, Side::Buy, 100, 4}}));
    EXPECT_EQ(e.book().find(1), (Order{1, Side::Sell, 100, 6}));
    // Next taker must hit order 1 (6 left) before order 2.
    EXPECT_EQ(e.submit(buy(4, 100, 8)),
              (Events{acc(buy(4, 100, 8)), Trade{1, 4, Side::Buy, 100, 6},
                      Trade{2, 4, Side::Buy, 100, 2}}));
    EXPECT_EQ(e.book().find(2), (Order{2, Side::Sell, 100, 8}));
    ExpectHealthy(e);
}

// ----------------------------------------------------------------- priorities --

TYPED_TEST(EngineTyped, PricePriority_BestPriceFirstRegardlessOfArrival) {
    TypeParam e;
    e.submit(sell(1, 102, 5));  // arrives first but worst price
    e.submit(sell(2, 100, 5));
    e.submit(sell(3, 101, 5));
    const Events ev = e.submit(buy(4, 105, 15));
    EXPECT_EQ(ev, (Events{acc(buy(4, 105, 15)), Trade{2, 4, Side::Buy, 100, 5},
                          Trade{3, 4, Side::Buy, 101, 5}, Trade{1, 4, Side::Buy, 102, 5}}));
    EXPECT_EQ(e.book().order_count(), 0u);
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, PricePriority_SellTakerHitsHighestBidFirst) {
    TypeParam e;
    e.submit(buy(1, 98, 5));
    e.submit(buy(2, 100, 5));
    e.submit(buy(3, 99, 5));
    const Events ev = e.submit(sell(4, 90, 15));
    EXPECT_EQ(ev, (Events{acc(sell(4, 90, 15)), Trade{2, 4, Side::Sell, 100, 5},
                          Trade{3, 4, Side::Sell, 99, 5}, Trade{1, 4, Side::Sell, 98, 5}}));
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, TimePriority_FifoWithinPrice_ArrivalNotIdOrder) {
    TypeParam e;
    e.submit(sell(30, 100, 5));
    e.submit(sell(10, 100, 5));
    e.submit(sell(20, 100, 5));
    const Events ev = e.submit(buy(99, 100, 12));
    EXPECT_EQ(ev, (Events{acc(buy(99, 100, 12)), Trade{30, 99, Side::Buy, 100, 5},
                          Trade{10, 99, Side::Buy, 100, 5}, Trade{20, 99, Side::Buy, 100, 2}}));
    EXPECT_EQ(e.book().find(20), (Order{20, Side::Sell, 100, 3}));
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, MultiLevelSweepStopsAtLimitAndRestsRemainder) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(sell(2, 101, 5));
    e.submit(sell(3, 103, 5));
    const Events ev = e.submit(buy(4, 102, 20));
    EXPECT_EQ(ev, (Events{acc(buy(4, 102, 20)), Trade{1, 4, Side::Buy, 100, 5},
                          Trade{2, 4, Side::Buy, 101, 5}}));
    EXPECT_EQ(e.book().find(4), (Order{4, Side::Buy, 102, 10}));
    EXPECT_EQ(e.book().best_ask(), 103);
    ExpectHealthy(e);
}

// ---------------------------------------------------------------- cancellation --

TYPED_TEST(EngineTyped, CancelRestingOrder) {
    TypeParam e;
    e.submit(buy(1, 100, 10));
    EXPECT_EQ(e.cancel(1), (Events{Cancelled{1, 10}}));
    EXPECT_EQ(e.book().order_count(), 0u);
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, CancelReportsRemainingAfterPartialFill) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    e.submit(buy(2, 100, 3));
    EXPECT_EQ(e.cancel(1), (Events{Cancelled{1, 7}}));
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, CancelledOrderCannotBeMatched) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    e.submit(sell(2, 100, 10));
    e.cancel(1);
    EXPECT_EQ(e.submit(buy(3, 100, 5)),
              (Events{acc(buy(3, 100, 5)), Trade{2, 3, Side::Buy, 100, 5}}));
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, CancelUnknownFilledOrAlreadyCancelledIsRejected) {
    TypeParam e;
    const Events unknown{Rejected{42, RejectReason::UnknownOrder}};
    EXPECT_EQ(e.cancel(42), unknown);

    e.submit(sell(1, 100, 5));
    e.submit(buy(2, 100, 5));  // fully fills order 1
    EXPECT_EQ(e.cancel(1), (Events{Rejected{1, RejectReason::UnknownOrder}}));
    EXPECT_EQ(e.cancel(2), (Events{Rejected{2, RejectReason::UnknownOrder}}));  // taker never rested

    e.submit(buy(3, 90, 5));
    EXPECT_EQ(e.cancel(3).size(), 1u);
    EXPECT_EQ(e.cancel(3), (Events{Rejected{3, RejectReason::UnknownOrder}}));
    ExpectHealthy(e);
}

// ------------------------------------------------------------------ rejections --

TYPED_TEST(EngineTyped, DuplicateLiveIdIsRejectedWithNoSideEffects) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    const BookSnapshot before = e.book().snapshot();
    // Would cross and trade if it were accepted.
    EXPECT_EQ(e.submit(buy(1, 100, 5)), (Events{Rejected{1, RejectReason::DuplicateId}}));
    EXPECT_EQ(e.book().snapshot(), before);
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, IdOfFullyFilledOrderMayBeReused) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(buy(2, 100, 5));
    EXPECT_EQ(e.submit(buy(1, 90, 3)), (Events{acc(buy(1, 90, 3))}));  // 1 is no longer live
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, MalformedOrdersAreRejected) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    EXPECT_EQ(e.submit(buy(2, 100, 0)), (Events{Rejected{2, RejectReason::InvalidQuantity}}));
    EXPECT_EQ(e.submit(buy(3, 0, 5)), (Events{Rejected{3, RejectReason::InvalidPrice}}));
    EXPECT_EQ(e.submit(buy(4, -1, 5)), (Events{Rejected{4, RejectReason::InvalidPrice}}));
    EXPECT_EQ(e.submit(buy(5, kMaxPrice + 1, 5)), (Events{Rejected{5, RejectReason::InvalidPrice}}));
    EXPECT_EQ(e.book().find(1), (Order{1, Side::Sell, 100, 5}));  // untouched
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, LevelOverflowRejectedBeforeAnyTrade) {
    TypeParam e;
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    e.submit(buy(1, 100, kMax));
    EXPECT_EQ(e.submit(buy(2, 100, 1)), (Events{Rejected{2, RejectReason::QuantityOverflow}}));
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, HugeQuantitiesTradeWithoutOverflow) {
    TypeParam e;
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    e.submit(sell(1, 100, kMax));
    const Events ev = e.submit(buy(2, 100, kMax));
    EXPECT_EQ(ev, (Events{acc(buy(2, 100, kMax)), Trade{1, 2, Side::Buy, 100, kMax}}));
    ExpectHealthy(e);
}

TYPED_TEST(EngineTyped, SelfTradeIsNotPrevented) {
    // Orders are anonymous; two orders that cross trade even if the same client sent both.
    TypeParam e;
    e.submit(buy(1, 100, 5));
    EXPECT_EQ(traded(e.submit(sell(2, 100, 5))), 5u);
}

TYPED_TEST(EngineTyped, EventsAppendToCallerBuffer) {
    TypeParam e;
    Events buf;
    e.submit(sell(1, 100, 5), buf);
    e.submit(buy(2, 100, 5), buf);
    e.cancel(9, buf);
    EXPECT_EQ(buf.size(), 1u + 2u + 1u);
}

TYPED_TEST(EngineTyped, EventsFormatAsText) {
    EXPECT_EQ(to_string(Event{Trade{1, 2, Side::Buy, 100, 5}}), "TRADE maker=1 taker=2 Buy 100 x 5");
    EXPECT_EQ(to_string(Event{Rejected{7, RejectReason::DuplicateId}}), "REJECTED id=7 DuplicateId");
}

// --------------------------------------------------- randomized vs naive model --

namespace {

// Deliberately naive reference implementation: a flat arrival-ordered vector and
// linear scans. Shares no code with the engine.
struct RefEngine {
    std::vector<Order> live;  // arrival order; partial fills keep position

    Events submit(Order o) {
        Events out;
        if (std::any_of(live.begin(), live.end(), [&](const Order& x) { return x.id == o.id; })) {
            out.push_back(Rejected{o.id, RejectReason::DuplicateId});
            return out;
        }
        out.push_back(Accepted{o.id, o.side, o.price, o.quantity});
        Quantity rem = o.quantity;
        while (rem > 0) {
            int best = -1;
            for (int i = 0; i < static_cast<int>(live.size()); ++i) {
                const Order& c = live[static_cast<std::size_t>(i)];
                if (c.side == o.side) continue;
                const bool ok = o.side == Side::Buy ? c.price <= o.price : c.price >= o.price;
                if (!ok) continue;
                if (best < 0) { best = i; continue; }
                const Order& b = live[static_cast<std::size_t>(best)];
                const bool better = o.side == Side::Buy ? c.price < b.price : c.price > b.price;
                if (better) best = i;  // ties keep the earlier arrival
            }
            if (best < 0) break;
            Order& m = live[static_cast<std::size_t>(best)];
            const Quantity q = std::min(rem, m.quantity);
            out.push_back(Trade{m.id, o.id, o.side, m.price, q});
            m.quantity -= q;
            rem -= q;
            if (m.quantity == 0) live.erase(live.begin() + best);
        }
        if (rem > 0) { o.quantity = rem; live.push_back(o); }
        return out;
    }

    Events cancel(OrderId id) {
        for (auto it = live.begin(); it != live.end(); ++it)
            if (it->id == id) {
                Events out{Cancelled{id, it->quantity}};
                live.erase(it);
                return out;
            }
        return {Rejected{id, RejectReason::UnknownOrder}};
    }
};

}  // namespace

TYPED_TEST(EngineTyped, RandomSequencesMatchNaiveModelAndPreserveInvariants) {
    for (std::uint32_t seed : {1u, 2u, 3u, 99u, 2024u}) {
        std::mt19937_64 rng(seed);
        TypeParam eng;
        RefEngine ref;
        OrderId next_id = 1;
        std::uint64_t total_submitted = 0, total_traded = 0;

        for (int step = 0; step < 1500; ++step) {
            Events got, want;
            Quantity submitted = 0;
            bool was_submit = false;
            Order o{};
            const auto roll = rng() % 100;
            if (roll < 30) {
                const OrderId id = 1 + rng() % (next_id + 2);  // live, dead, or never-seen
                got = eng.cancel(id);
                want = ref.cancel(id);
            } else {
                // Narrow price band so most orders cross; occasionally reuse an ID.
                const OrderId id = (roll > 95 && next_id > 1) ? 1 + rng() % next_id : next_id++;
                o = Order{id, (rng() & 1) ? Side::Buy : Side::Sell,
                          static_cast<Price>(95 + rng() % 11), 1 + rng() % 20};
                got = eng.submit(o);
                want = ref.submit(o);
                submitted = o.quantity;
                was_submit = true;
            }
            ASSERT_EQ(got, want) << "seed " << seed << " step " << step;

            std::string why;
            ASSERT_TRUE(eng.book().validate(&why)) << "seed " << seed << ": " << why;
            ASSERT_FALSE(eng.book().is_crossed()) << "seed " << seed << " step " << step;
            ASSERT_EQ(eng.book().order_count(), ref.live.size());

            if (was_submit && std::holds_alternative<Accepted>(got.front())) {
                // Conservation: traded + resting remainder == submitted.
                const Quantity t = traded(got);
                const auto rest = eng.book().find(o.id);
                ASSERT_EQ(t + (rest ? rest->quantity : 0), submitted)
                    << "seed " << seed << " step " << step;
                ASSERT_LE(t, submitted);
                total_submitted += submitted;
                total_traded += t;
            }
        }
        EXPECT_GT(total_traded, 0u) << "seed " << seed << " never crossed; test is vacuous";
        EXPECT_LE(total_traded, total_submitted);
    }
}

TYPED_TEST(EngineTyped, SameInputProducesIdenticalEventStream) {
    auto run = [] {
        std::mt19937_64 rng(31337);
        TypeParam e;
        Events all;
        for (OrderId id = 1; id <= 800; ++id) {
            e.submit(Order{id, (rng() & 1) ? Side::Buy : Side::Sell,
                           static_cast<Price>(95 + rng() % 11), 1 + rng() % 20}, all);
            if (rng() % 4 == 0) e.cancel(1 + rng() % id, all);
        }
        return all;
    };
    EXPECT_EQ(run(), run());
}
