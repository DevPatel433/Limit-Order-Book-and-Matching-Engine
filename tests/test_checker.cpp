// The checker is the oracle for the property tests, so it needs its own tests:
// it must accept real engine output and reject each kind of fabricated violation.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "lob/checker.hpp"

using namespace lob;
using Events = std::vector<Event>;

namespace {

Order buy(OrderId id, Price p, Quantity q) { return Order{id, Side::Buy, p, q}; }
Order sell(OrderId id, Price p, Quantity q) { return Order{id, Side::Sell, p, q}; }

// Drives a real engine and a checker in lock-step.
template <class E = MatchingEngine>
struct Harness {
    E engine;
    ConsistencyChecker checker;

    std::optional<std::string> submit(const Order& o) { return checker.observe(engine.submit(o), engine); }
    std::optional<std::string> cancel(OrderId id) { return checker.observe(engine.cancel(id), engine); }
};

}  // namespace

TEST(Checker, AcceptsRealEngineOutput) {
    Harness<> h;
    for (const Order& o : {sell(1, 100, 5), sell(2, 100, 5), sell(3, 101, 5), buy(4, 99, 3), buy(5, 101, 12),
                           sell(6, 98, 9), buy(7, 100, 1), buy(5, 1, 1) /* duplicate */})
        EXPECT_EQ(h.submit(o), std::nullopt) << "order " << o.id;
    EXPECT_EQ(h.cancel(7), std::nullopt);
    EXPECT_EQ(h.cancel(7), std::nullopt);  // unknown
    EXPECT_EQ(h.cancel(999), std::nullopt);
}

TEST(Checker, AcceptsPooledEngineOutputToo) {
    Harness<PooledMatchingEngine> h;
    for (const Order& o : {sell(1, 100, 5), sell(2, 100, 5), sell(3, 101, 5), buy(4, 99, 3), buy(5, 101, 12),
                           sell(6, 98, 9), buy(7, 100, 1), buy(5, 1, 1)})
        EXPECT_EQ(h.submit(o), std::nullopt) << "order " << o.id;
    EXPECT_EQ(h.cancel(7), std::nullopt);
    EXPECT_EQ(h.cancel(999), std::nullopt);
}

// Sets up two sells (1@100 x5, 2@101 x5), then feeds a hand-made event batch
// that a buggy engine might emit, and expects the checker to object.
class CheckerRejects : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(h.submit(sell(1, 100, 5)), std::nullopt);
        ASSERT_EQ(h.submit(sell(2, 101, 5)), std::nullopt);
    }
    std::optional<std::string> feed(Events ev) { return h.checker.observe(ev, h.engine); }
    Harness<> h;
};

TEST_F(CheckerRejects, PricePriorityViolation) {
    auto v = feed({Accepted{3, Side::Buy, 105, 5}, Trade{2, 3, Side::Buy, 101, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("price priority"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, TradeAtTakerPriceInsteadOfMakerPrice) {
    auto v = feed({Accepted{3, Side::Buy, 105, 5}, Trade{1, 3, Side::Buy, 105, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("maker's price"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, TradeBeyondTakersLimit) {
    auto v = feed({Accepted{3, Side::Buy, 99, 5}, Trade{1, 3, Side::Buy, 100, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("limit"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, OverfillingTheMaker) {
    auto v = feed({Accepted{3, Side::Buy, 100, 9}, Trade{1, 3, Side::Buy, 100, 6}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("maker's remaining"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, OverfillingTheTaker) {
    auto v = feed({Accepted{3, Side::Buy, 100, 2}, Trade{1, 3, Side::Buy, 100, 4}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("taker's remaining"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, TradeAgainstUnknownMaker) {
    auto v = feed({Accepted{3, Side::Buy, 100, 2}, Trade{77, 3, Side::Buy, 100, 2}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("non-live maker"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, TradeWithoutAccepted) {
    EXPECT_TRUE(feed({Trade{1, 3, Side::Buy, 100, 2}}));
}

TEST_F(CheckerRejects, WrongCancelledQuantity) {
    auto v = feed({Cancelled{1, 4}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("remaining"), std::string::npos) << *v;
}

TEST_F(CheckerRejects, UnjustifiedRejections) {
    EXPECT_TRUE(feed({Rejected{42, RejectReason::DuplicateId}}));   // 42 is not live
    EXPECT_TRUE(feed({Rejected{1, RejectReason::UnknownOrder}}));   // 1 is live
}

TEST_F(CheckerRejects, EmptyEventBatch) { EXPECT_TRUE(feed({})); }

TEST_F(CheckerRejects, EventsThatDisagreeWithTheEngineBook) {
    // Perfectly legal events, but the engine never executed them, so its book differs.
    auto v = feed({Accepted{3, Side::Buy, 100, 5}, Trade{1, 3, Side::Buy, 100, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("ledger"), std::string::npos) << *v;
}

TEST(Checker, DetectsFifoViolationInEventStream) {
    Harness<> h;
    ASSERT_EQ(h.submit(sell(1, 100, 5)), std::nullopt);
    ASSERT_EQ(h.submit(sell(2, 100, 5)), std::nullopt);
    auto v = h.checker.observe({Accepted{3, Side::Buy, 100, 5}, Trade{2, 3, Side::Buy, 100, 5}}, h.engine);
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("time priority"), std::string::npos) << *v;
}

// ------------------------------------------------------------ Phase 6 semantics ----------

TEST(Checker, AcceptsRealOutputForAllOrderTypesOnBothEngines) {
    auto drive = [](auto& h) {
        const std::vector<Order> orders{
            sell(1, 100, 5), sell(2, 101, 5), sell(3, 101, 5),
            Order(4, Side::Buy, 99, 10, TimeInForce::IOC),      // expires untouched
            Order(5, Side::Buy, 101, 7, TimeInForce::FOK),      // fills across two levels
            Order(6, Side::Buy, 101, 99, TimeInForce::FOK),     // killed
            Order::market(7, Side::Buy, 4),                     // partial then expires
            Order::market(8, Side::Sell, 3),                    // empty bid side: expires
            Order::market(9, Side::Buy, 3, TimeInForce::GTC),   // rejected
            buy(10, 90, 5), buy(11, 90, 5),
        };
        for (const Order& o : orders) EXPECT_EQ(h.submit(o), std::nullopt) << "order " << o.id;
        EXPECT_EQ(h.checker.observe(h.engine.amend(10, 90, 2), h.engine), std::nullopt);   // kept
        EXPECT_EQ(h.checker.observe(h.engine.amend(11, 91, 5), h.engine), std::nullopt);   // lost
        EXPECT_EQ(h.checker.observe(h.engine.amend(11, 200, 5), h.engine), std::nullopt);  // crosses/moves
        EXPECT_EQ(h.checker.observe(h.engine.amend(999, 100, 5), h.engine), std::nullopt); // unknown
    };
    Harness<> a;
    drive(a);
    Harness<PooledMatchingEngine> b;
    drive(b);
}

class CheckerRejectsPhase6 : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(h.submit(sell(1, 100, 5)), std::nullopt);
        ASSERT_EQ(h.submit(sell(2, 101, 5)), std::nullopt);
        ASSERT_EQ(h.submit(buy(3, 90, 5)), std::nullopt);
    }
    std::optional<std::string> feed(Events ev) { return h.checker.observe(ev, h.engine); }
    Harness<> h;
};

TEST_F(CheckerRejectsPhase6, IocRemainderNeitherFilledNorExpired) {
    auto v = feed({Accepted{4, Side::Buy, 99, 5, TimeInForce::IOC}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("neither filled nor expired"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, ExpiredWithWrongRemaining) {
    auto v = feed({Accepted{4, Side::Buy, 99, 5, TimeInForce::IOC}, Expired{4, 3}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("wrong remaining"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, ExpiredForAGtcOrder) {
    auto v = feed({Accepted{4, Side::Buy, 99, 5}, Expired{4, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("GTC"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, ExpiredWithoutAnOpenOrder) { EXPECT_TRUE(feed({Expired{4, 5}})); }

TEST_F(CheckerRejectsPhase6, EventAfterExpired) {
    auto v = feed({Accepted{4, Side::Buy, 99, 5, TimeInForce::IOC}, Expired{4, 5}, Cancelled{3, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("after Expired"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, FokPartiallyFilled) {
    auto v = feed({Accepted{4, Side::Buy, 100, 9, TimeInForce::FOK}, Trade{1, 4, Side::Buy, 100, 5}, Expired{4, 4}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("FOK"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, FokKilledAlthoughFillable) {
    auto v = feed({Accepted{4, Side::Buy, 100, 5, TimeInForce::FOK}, Expired{4, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("fully fillable"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, IocExpiredWhileCrossingLiquidityRemains) {
    auto v = feed({Accepted{4, Side::Buy, 100, 5, TimeInForce::IOC}, Expired{4, 5}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("crossing liquidity"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, MarketOrderThatLeavesLiquidityBehind) {
    auto v = feed({Accepted{4, Side::Buy, 0, 3, TimeInForce::IOC, OrderType::Market}, Expired{4, 3}});
    ASSERT_TRUE(v);
    EXPECT_NE(v->find("crossing liquidity"), std::string::npos) << *v;
}

TEST_F(CheckerRejectsPhase6, GtcMarketOrderAccepted) {
    EXPECT_TRUE(feed({Accepted{4, Side::Buy, 0, 3, TimeInForce::GTC, OrderType::Market}}));
}

TEST_F(CheckerRejectsPhase6, AmendedWithWrongOldValues) {
    EXPECT_TRUE(feed({Amended{3, 91, 5, 91, 4, true}}));  // old price is not 90
    EXPECT_TRUE(feed({Amended{3, 90, 6, 90, 4, true}}));  // old quantity is not 5
}

TEST_F(CheckerRejectsPhase6, AmendedWithWrongPriorityFlag) {
    auto lost_for_pure_decrease = feed({Amended{3, 90, 5, 90, 3, false}});
    ASSERT_TRUE(lost_for_pure_decrease);
    EXPECT_NE(lost_for_pure_decrease->find("priority"), std::string::npos) << *lost_for_pure_decrease;
}

TEST_F(CheckerRejectsPhase6, AmendedClaimsKeptForAPriceChange) {
    EXPECT_TRUE(feed({Amended{3, 90, 5, 91, 5, true}}));
}

TEST_F(CheckerRejectsPhase6, AmendedNonLiveOrderOrToZero) {
    EXPECT_TRUE(feed({Amended{42, 90, 5, 90, 3, true}}));
    EXPECT_TRUE(feed({Amended{3, 90, 5, 90, 0, true}}));
}
