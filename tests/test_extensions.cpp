// Tests for the Phase 6 order semantics: IOC / FOK time in force, market orders, and amend.
// Every typed test runs against both the baseline and the pooled engine.
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "lob/generator.hpp"
#include "lob/matching_engine.hpp"
#include "lob/replay.hpp"

using namespace lob;
using Events = std::vector<Event>;

template <class E>
class ExtTyped : public ::testing::Test {};
using BothEngines = ::testing::Types<MatchingEngine, PooledMatchingEngine>;
TYPED_TEST_SUITE(ExtTyped, BothEngines);

namespace {

Order buy(OrderId id, Price p, Quantity q) { return Order(id, Side::Buy, p, q); }
Order sell(OrderId id, Price p, Quantity q) { return Order(id, Side::Sell, p, q); }
Order ioc(Side s, OrderId id, Price p, Quantity q) { return Order(id, s, p, q, TimeInForce::IOC); }
Order fok(Side s, OrderId id, Price p, Quantity q) { return Order(id, s, p, q, TimeInForce::FOK); }
Accepted acc(const Order& o) { return Accepted{o.id, o.side, o.price, o.quantity, o.tif, o.type}; }

template <class E>
void ExpectHealthy(const E& e) {
    std::string why;
    EXPECT_TRUE(e.book().validate(&why)) << why;
    EXPECT_FALSE(e.book().is_crossed());
}

template <class E>
std::vector<OrderId> queue_ids(const E& e, Side s, Price p) {
    std::vector<OrderId> out;
    if (const auto* l = e.book().level(s, p))
        for (const Order& o : l->orders()) out.push_back(o.id);
    return out;
}

}  // namespace

// ================================================================== IOC ==========

TYPED_TEST(ExtTyped, IocFullyFilledHasNoExpiry) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    const Order o = ioc(Side::Buy, 2, 100, 5);
    EXPECT_EQ(e.submit(o), (Events{acc(o), Trade{1, 2, Side::Buy, 100, 5}}));
    EXPECT_EQ(e.book().order_count(), 0u);
}

TYPED_TEST(ExtTyped, IocPartialFillExpiresTheRemainderAndNeverRests) {
    TypeParam e;
    e.submit(sell(1, 100, 4));
    const Order o = ioc(Side::Buy, 2, 100, 10);
    EXPECT_EQ(e.submit(o), (Events{acc(o), Trade{1, 2, Side::Buy, 100, 4}, Expired{2, 6}}));
    EXPECT_FALSE(e.book().contains(2));
    EXPECT_EQ(e.book().order_count(), 0u);
    EXPECT_FALSE(e.book().best_bid());  // nothing rested at the IOC's limit
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, IocRespectsItsLimitPrice) {
    TypeParam e;
    e.submit(sell(1, 101, 5));
    const Order o = ioc(Side::Buy, 2, 100, 5);
    EXPECT_EQ(e.submit(o), (Events{acc(o), Expired{2, 5}}));
    EXPECT_EQ(e.book().find(1), (Order{1, Side::Sell, 101, 5}));  // untouched
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, IocSweepsLevelsInPriceOrderThenExpires) {
    TypeParam e;
    e.submit(sell(1, 102, 3));
    e.submit(sell(2, 100, 3));
    e.submit(sell(3, 101, 3));
    const Order o = ioc(Side::Buy, 4, 101, 20);  // limit 101: must not touch 102
    EXPECT_EQ(e.submit(o), (Events{acc(o), Trade{2, 4, Side::Buy, 100, 3}, Trade{3, 4, Side::Buy, 101, 3}, Expired{4, 14}}));
    EXPECT_EQ(e.book().best_ask(), 102);
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, IocWithNoLiquidityIsAcceptedThenExpired) {
    TypeParam e;
    const Order o = ioc(Side::Sell, 1, 100, 7);
    EXPECT_EQ(e.submit(o), (Events{acc(o), Expired{1, 7}}));
    EXPECT_EQ(e.book().order_count(), 0u);
}

TYPED_TEST(ExtTyped, IocDuplicateLiveIdIsRejectedBeforeTrading) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    EXPECT_EQ(e.submit(ioc(Side::Buy, 1, 100, 5)), (Events{Rejected{1, RejectReason::DuplicateId}}));
    EXPECT_EQ(e.book().find(1), (Order{1, Side::Sell, 100, 5}));
}

TYPED_TEST(ExtTyped, IdOfAnExpiredIocCanBeReused) {
    TypeParam e;
    e.submit(ioc(Side::Buy, 1, 100, 5));
    EXPECT_EQ(e.submit(buy(1, 90, 5)), (Events{acc(buy(1, 90, 5))}));
}

// ================================================================== FOK ==========

TYPED_TEST(ExtTyped, FokFillsWhenFullyAvailableAcrossLevels) {
    TypeParam e;
    e.submit(sell(1, 100, 4));
    e.submit(sell(2, 101, 4));
    const Order o = fok(Side::Buy, 3, 101, 8);
    EXPECT_EQ(e.submit(o), (Events{acc(o), Trade{1, 3, Side::Buy, 100, 4}, Trade{2, 3, Side::Buy, 101, 4}}));
    EXPECT_EQ(e.book().order_count(), 0u);
}

TYPED_TEST(ExtTyped, FokKillsUntouchedWhenNotFullyFillable) {
    TypeParam e;
    e.submit(sell(1, 100, 4));
    e.submit(sell(2, 101, 4));
    const BookSnapshot before = e.book().snapshot();
    const Order o = fok(Side::Buy, 3, 101, 9);  // 8 available
    EXPECT_EQ(e.submit(o), (Events{acc(o), Expired{3, 9}}));  // no trades at all
    EXPECT_EQ(e.book().snapshot(), before);
    EXPECT_EQ(e.book().find(1), (Order{1, Side::Sell, 100, 4}));
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, FokExactBoundary) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    const Order exact = fok(Side::Buy, 2, 100, 10);
    EXPECT_EQ(e.submit(exact), (Events{acc(exact), Trade{1, 2, Side::Buy, 100, 10}}));
    e.submit(sell(3, 100, 10));
    const Order over = fok(Side::Buy, 4, 100, 11);
    EXPECT_EQ(e.submit(over), (Events{acc(over), Expired{4, 11}}));
    EXPECT_EQ(e.book().quantity_at(Side::Sell, 100), 10u);
}

TYPED_TEST(ExtTyped, FokDoesNotCountLiquidityBeyondItsLimit) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(sell(2, 105, 50));  // plenty, but beyond the limit
    const Order o = fok(Side::Buy, 3, 101, 6);
    EXPECT_EQ(e.submit(o), (Events{acc(o), Expired{3, 6}}));
}

TYPED_TEST(ExtTyped, FokSellSideIsSymmetric) {
    TypeParam e;
    e.submit(buy(1, 100, 3));
    e.submit(buy(2, 99, 3));
    const Order ok = fok(Side::Sell, 3, 99, 6);
    EXPECT_EQ(e.submit(ok), (Events{acc(ok), Trade{1, 3, Side::Sell, 100, 3}, Trade{2, 3, Side::Sell, 99, 3}}));
    e.submit(buy(4, 100, 3));
    const Order no = fok(Side::Sell, 5, 100, 4);
    EXPECT_EQ(e.submit(no), (Events{acc(no), Expired{5, 4}}));
}

TYPED_TEST(ExtTyped, FokIgnoresSameSideOrders) {
    TypeParam e;
    e.submit(buy(1, 100, 50));
    const Order o = fok(Side::Buy, 2, 100, 5);  // only bids on the book: nothing to buy
    EXPECT_EQ(e.submit(o), (Events{acc(o), Expired{2, 5}}));
}

// ================================================================ market =========

TYPED_TEST(ExtTyped, MarketBuySweepsAtMakerPricesWithNoLimit) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(sell(2, 5000, 5));
    const Order m = Order::market(3, Side::Buy, 6);
    EXPECT_EQ(e.submit(m), (Events{acc(m), Trade{1, 3, Side::Buy, 100, 5}, Trade{2, 3, Side::Buy, 5000, 1}}));
    EXPECT_EQ(e.book().find(2), (Order{2, Side::Sell, 5000, 4}));
}

TYPED_TEST(ExtTyped, MarketSellSweepsBidsDownward) {
    TypeParam e;
    e.submit(buy(1, 100, 2));
    e.submit(buy(2, 1, 5));  // absurdly low bid: a market sell still takes it
    const Order m = Order::market(3, Side::Sell, 6);
    EXPECT_EQ(e.submit(m), (Events{acc(m), Trade{1, 3, Side::Sell, 100, 2}, Trade{2, 3, Side::Sell, 1, 4}}));
}

TYPED_TEST(ExtTyped, MarketRemainderExpiresAndNeverRests) {
    TypeParam e;
    e.submit(sell(1, 100, 4));
    const Order m = Order::market(2, Side::Buy, 10);
    EXPECT_EQ(e.submit(m), (Events{acc(m), Trade{1, 2, Side::Buy, 100, 4}, Expired{2, 6}}));
    EXPECT_EQ(e.book().order_count(), 0u);
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, MarketOnAnEmptySideIsAcceptedThenExpired) {
    TypeParam e;
    e.submit(buy(1, 100, 5));  // only the same side has orders
    const Order m = Order::market(2, Side::Buy, 3);
    EXPECT_EQ(e.submit(m), (Events{acc(m), Expired{2, 3}}));
}

TYPED_TEST(ExtTyped, MarketFokIsAllOrNothing) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    const Order too_big = Order::market(2, Side::Buy, 6, TimeInForce::FOK);
    EXPECT_EQ(e.submit(too_big), (Events{acc(too_big), Expired{2, 6}}));
    const Order fits = Order::market(3, Side::Buy, 5, TimeInForce::FOK);
    EXPECT_EQ(e.submit(fits), (Events{acc(fits), Trade{1, 3, Side::Buy, 100, 5}}));
}

TYPED_TEST(ExtTyped, MarketOrderValidation) {
    TypeParam e;
    EXPECT_EQ(e.submit(Order::market(1, Side::Buy, 5, TimeInForce::GTC)),
              (Events{Rejected{1, RejectReason::InvalidTimeInForce}}));  // could never rest
    EXPECT_EQ(e.submit(Order::market(2, Side::Buy, 0)), (Events{Rejected{2, RejectReason::InvalidQuantity}}));
    // The price field of a market order is ignored, whatever it holds.
    const Order odd(3, Side::Buy, -999, 5, TimeInForce::IOC, OrderType::Market);
    EXPECT_EQ(e.submit(odd), (Events{acc(odd), Expired{3, 5}}));
    EXPECT_EQ(e.book().order_count(), 0u);
}

TYPED_TEST(ExtTyped, LimitIocAndFokStillValidateTheirPrice) {
    TypeParam e;
    EXPECT_EQ(e.submit(ioc(Side::Buy, 1, 0, 5)), (Events{Rejected{1, RejectReason::InvalidPrice}}));
    EXPECT_EQ(e.submit(fok(Side::Sell, 2, kMaxPrice + 1, 5)), (Events{Rejected{2, RejectReason::InvalidPrice}}));
}

namespace {
template <class Book>
void expect_book_add_is_resting_only() {
    Book b;
    EXPECT_EQ(b.add(ioc(Side::Buy, 1, 100, 5)), AddStatus::InvalidTimeInForce);
    EXPECT_EQ(b.add(fok(Side::Buy, 2, 100, 5)), AddStatus::InvalidTimeInForce);
    EXPECT_EQ(b.add(Order::market(3, Side::Buy, 5)), AddStatus::InvalidTimeInForce);
    EXPECT_EQ(b.add(Order::market(4, Side::Buy, 5, TimeInForce::GTC)), AddStatus::InvalidTimeInForce);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.add(Order(5, Side::Buy, 100, 5)), AddStatus::Ok);  // GTC limit is fine
}
}  // namespace

TEST(ExtBook, OnlyGtcLimitOrdersCanRest) {
    expect_book_add_is_resting_only<OrderBook>();
    expect_book_add_is_resting_only<PooledOrderBook>();
}

TEST(ExtBook, FillableQuantityCountsOnlyAcceptablePricesAndStopsAtNeeded) {
    OrderBook b;
    b.add(Order(1, Side::Sell, 100, 4));
    b.add(Order(2, Side::Sell, 101, 4));
    b.add(Order(3, Side::Sell, 105, 100));
    b.add(Order(4, Side::Buy, 90, 7));
    EXPECT_EQ(b.fillable_quantity(Side::Buy, 100, 1000), 4u);
    EXPECT_EQ(b.fillable_quantity(Side::Buy, 101, 1000), 8u);
    EXPECT_EQ(b.fillable_quantity(Side::Buy, 99, 1000), 0u);
    EXPECT_GE(b.fillable_quantity(Side::Buy, 105, 6), 6u);     // may stop early once enough is found
    EXPECT_EQ(b.fillable_quantity(Side::Sell, 90, 1000), 7u);  // a seller consumes bids
    EXPECT_EQ(b.fillable_quantity(Side::Sell, 91, 1000), 0u);
}

// ================================================================= amend =========

TYPED_TEST(ExtTyped, AmendRejectsUnknownZeroQuantityAndBadPrice) {
    TypeParam e;
    e.submit(buy(1, 100, 5));
    const BookSnapshot before = e.book().snapshot();
    EXPECT_EQ(e.amend(42, 100, 5), (Events{Rejected{42, RejectReason::UnknownOrder}}));
    EXPECT_EQ(e.amend(1, 100, 0), (Events{Rejected{1, RejectReason::InvalidQuantity}}));
    EXPECT_EQ(e.amend(1, 0, 5), (Events{Rejected{1, RejectReason::InvalidPrice}}));
    EXPECT_EQ(e.amend(1, kMaxPrice + 1, 5), (Events{Rejected{1, RejectReason::InvalidPrice}}));
    EXPECT_EQ(e.book().snapshot(), before);
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, AmendOfFilledOrCancelledOrderIsUnknown) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(buy(2, 100, 5));  // fills order 1
    EXPECT_EQ(e.amend(1, 100, 3), (Events{Rejected{1, RejectReason::UnknownOrder}}));
    e.submit(buy(3, 90, 5));
    e.cancel(3);
    EXPECT_EQ(e.amend(3, 90, 3), (Events{Rejected{3, RejectReason::UnknownOrder}}));
}

TYPED_TEST(ExtTyped, DecreasingQuantityKeepsTimePriority) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    e.submit(sell(2, 100, 10));
    EXPECT_EQ(e.amend(1, 100, 3), (Events{Amended{1, 100, 10, 100, 3, true}}));
    EXPECT_EQ(queue_ids(e, Side::Sell, 100), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(e.book().quantity_at(Side::Sell, 100), 13u);
    const Order taker = buy(3, 100, 5);
    // Order 1 is still first: 3 from it, then 2 from order 2.
    EXPECT_EQ(e.submit(taker), (Events{acc(taker), Trade{1, 3, Side::Buy, 100, 3}, Trade{2, 3, Side::Buy, 100, 2}}));
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, IncreasingQuantityLosesTimePriority) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(sell(2, 100, 5));
    EXPECT_EQ(e.amend(1, 100, 8), (Events{Amended{1, 100, 5, 100, 8, false}}));
    EXPECT_EQ(queue_ids(e, Side::Sell, 100), (std::vector<OrderId>{2, 1}));  // moved to the back
    const Order taker = buy(3, 100, 6);
    EXPECT_EQ(e.submit(taker), (Events{acc(taker), Trade{2, 3, Side::Buy, 100, 5}, Trade{1, 3, Side::Buy, 100, 1}}));
    EXPECT_EQ(e.book().find(1), (Order{1, Side::Sell, 100, 7}));
}

TYPED_TEST(ExtTyped, AmendToIdenticalValuesIsAPriorityKeepingNoOp) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(sell(2, 100, 5));
    EXPECT_EQ(e.amend(1, 100, 5), (Events{Amended{1, 100, 5, 100, 5, true}}));
    EXPECT_EQ(queue_ids(e, Side::Sell, 100), (std::vector<OrderId>{1, 2}));
}

TYPED_TEST(ExtTyped, PriceChangeMovesLevelAndGoesToTheBackOfTheNewQueue) {
    TypeParam e;
    e.submit(buy(1, 99, 5));
    e.submit(buy(2, 100, 5));
    e.submit(buy(3, 100, 5));
    EXPECT_EQ(e.amend(1, 100, 5), (Events{Amended{1, 99, 5, 100, 5, false}}));
    EXPECT_EQ(e.book().level(Side::Buy, 99), nullptr);  // old level disappeared
    EXPECT_EQ(queue_ids(e, Side::Buy, 100), (std::vector<OrderId>{2, 3, 1}));
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, AmendingBackToAnOldPriceDoesNotRestoreTheOldPosition) {
    TypeParam e;
    e.submit(buy(1, 100, 5));
    e.submit(buy(2, 100, 5));
    e.amend(1, 101, 5);
    e.amend(1, 100, 5);
    EXPECT_EQ(queue_ids(e, Side::Buy, 100), (std::vector<OrderId>{2, 1}));
}

TYPED_TEST(ExtTyped, AmendIntoTheSpreadCrossesAndTradesAsTaker) {
    TypeParam e;
    e.submit(sell(1, 100, 5));
    e.submit(buy(2, 90, 8));
    EXPECT_EQ(e.amend(2, 100, 8), (Events{Amended{2, 90, 8, 100, 8, false}, Trade{1, 2, Side::Buy, 100, 5}}));
    EXPECT_EQ(e.book().find(2), (Order{2, Side::Buy, 100, 3}));  // remainder rests at the new price
    EXPECT_EQ(e.book().best_bid(), 100);
    EXPECT_FALSE(e.book().best_ask());
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, SellAmendedDownThroughTheBidsTradesAtBidPrices) {
    TypeParam e;
    e.submit(buy(1, 100, 4));
    e.submit(buy(2, 99, 4));
    e.submit(sell(3, 110, 6));
    EXPECT_EQ(e.amend(3, 99, 6), (Events{Amended{3, 110, 6, 99, 6, false}, Trade{1, 3, Side::Sell, 100, 4},
                                         Trade{2, 3, Side::Sell, 99, 2}}));
    EXPECT_EQ(e.book().order_count(), 1u);  // order 2 has 2 left
    EXPECT_EQ(e.book().find(2), (Order{2, Side::Buy, 99, 2}));
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, AmendWorksOnOpenQuantityAfterAPartialFill) {
    TypeParam e;
    e.submit(buy(1, 100, 10));
    e.submit(sell(2, 100, 4));  // order 1 now has 6 open
    EXPECT_EQ(e.amend(1, 100, 6), (Events{Amended{1, 100, 6, 100, 6, true}}));   // same as open: kept
    EXPECT_EQ(e.amend(1, 100, 7), (Events{Amended{1, 100, 6, 100, 7, false}}));  // above open: lost
}

TYPED_TEST(ExtTyped, AmendedOrderStillTradesAndCancelsNormally) {
    TypeParam e;
    e.submit(sell(1, 100, 10));
    e.amend(1, 101, 6);
    const Order t = buy(2, 101, 4);
    EXPECT_EQ(e.submit(t), (Events{acc(t), Trade{1, 2, Side::Buy, 101, 4}}));
    EXPECT_EQ(e.cancel(1), (Events{Cancelled{1, 2}}));
    ExpectHealthy(e);
}

TYPED_TEST(ExtTyped, AmendThatWouldOverflowALevelIsRejectedAndKeepsTheOriginal) {
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    {
        TypeParam e;
        e.submit(buy(1, 100, kMax - 5));
        e.submit(buy(2, 99, 1));
        const BookSnapshot before = e.book().snapshot();
        EXPECT_EQ(e.amend(2, 100, 10), (Events{Rejected{2, RejectReason::QuantityOverflow}}));
        EXPECT_EQ(e.book().snapshot(), before);
        EXPECT_EQ(e.book().find(2), (Order{2, Side::Buy, 99, 1}));  // not lost
        ExpectHealthy(e);
    }
    {
        // Growing within its own level: the old quantity leaves the level first, so this one fits...
        TypeParam e;
        e.submit(buy(1, 100, kMax - 5));
        e.submit(buy(2, 100, 5));
        EXPECT_EQ(e.amend(2, 100, 5), (Events{Amended{2, 100, 5, 100, 5, true}}));
        // ...and one more than the level can hold does not.
        EXPECT_EQ(e.amend(2, 100, 6), (Events{Rejected{2, RejectReason::QuantityOverflow}}));
        EXPECT_EQ(e.book().find(2), (Order{2, Side::Buy, 100, 5}));
        ExpectHealthy(e);
    }
}

// ============================================================ formats ==========

TEST(ExtFormats, EventTextForms) {
    EXPECT_EQ(to_string(Event{Accepted{1, Side::Buy, 100, 5}}), "ACCEPTED id=1 Buy 100 x 5");  // unchanged
    EXPECT_EQ(to_string(Event{Accepted{1, Side::Buy, 100, 5, TimeInForce::IOC}}), "ACCEPTED id=1 Buy 100 x 5 IOC");
    EXPECT_EQ(to_string(Event{Accepted{1, Side::Sell, 0, 5, TimeInForce::FOK, OrderType::Market}}),
              "ACCEPTED id=1 Sell MKT x 5 FOK");
    EXPECT_EQ(to_string(Event{Expired{3, 7}}), "EXPIRED id=3 remaining=7");
    EXPECT_EQ(to_string(Event{Amended{4, 100, 5, 101, 5, false}}), "AMENDED id=4 100 x 5 -> 101 x 5 priority=lost");
    EXPECT_EQ(to_string(Event{Amended{4, 100, 5, 100, 2, true}}), "AMENDED id=4 100 x 5 -> 100 x 2 priority=kept");
    EXPECT_EQ(to_string(Event{Rejected{9, RejectReason::InvalidTimeInForce}}), "REJECTED id=9 InvalidTimeInForce");
}

TEST(ExtFormats, CsvRowsHaveExactlyTheHeaderColumns) {
    const std::string header = csv_header();
    const auto cols = [](const std::string& s) { return std::count(s.begin(), s.end(), ',') + 1; };
    ASSERT_EQ(cols(header), 11);
    const std::vector<Event> all{Accepted{1, Side::Buy, 100, 5}, Accepted{2, Side::Sell, 0, 5, TimeInForce::IOC, OrderType::Market},
                                 Trade{1, 2, Side::Buy, 100, 5}, Cancelled{3, 4}, Expired{5, 6},
                                 Amended{7, 100, 5, 101, 6, false}, Rejected{8, RejectReason::DuplicateId}};
    for (const Event& e : all) EXPECT_EQ(cols(to_csv_row(1, 1, e)), 11) << to_string(e);
}

TEST(ExtFormats, CsvRowContents) {
    EXPECT_EQ(to_csv_row(1, 1, Event{Trade{1, 2, Side::Buy, 100, 5}}), "1,1,TRADE,2,Buy,100,5,1,2,,");
    EXPECT_EQ(to_csv_row(2, 3, Event{Accepted{4, Side::Sell, 0, 9, TimeInForce::IOC, OrderType::Market}}),
              "2,3,ACCEPTED,4,Sell,MKT,9,,,,IOC");
    EXPECT_EQ(to_csv_row(3, 4, Event{Expired{5, 6}}), "3,4,EXPIRED,5,,,,,,6,");
    EXPECT_EQ(to_csv_row(4, 5, Event{Amended{7, 100, 5, 101, 6, false}}), "4,5,AMENDED,7,,101,6,,,,100:5:lost");
    EXPECT_EQ(to_csv_row(5, 6, Event{Rejected{8, RejectReason::DuplicateId}}), "5,6,REJECTED,8,,,,,,,DuplicateId");
    EXPECT_EQ(to_csv_row(6, 7, Event{Cancelled{3, 4}}), "6,7,CANCELLED,3,,,,,,4,");
}

// ============================================== randomized vs naive model ==========

namespace {

// Deliberately naive reference for the extended semantics: a flat vector and linear scans.
// Shares no code with the engine.
struct Ref {
    struct R {
        Order o;
        std::uint64_t seq;
    };
    std::vector<R> live;
    std::uint64_t next_seq = 0;

    int find(OrderId id) const {
        for (std::size_t i = 0; i < live.size(); ++i)
            if (live[i].o.id == id) return static_cast<int>(i);
        return -1;
    }
    // Independent of effective_limit() in order.hpp on purpose (see the checker).
    static Price limit_of(const Order& o) {
        if (o.type == OrderType::Limit) return o.price;
        return o.side == Side::Buy ? kMaxPrice : kMinPrice;
    }
    bool crosses(Side taker, Price limit, Price resting) const { return taker == Side::Buy ? resting <= limit : resting >= limit; }

    int best(Side taker, Price limit) const {  // index of the best crossing opposite order, or -1
        int b = -1;
        for (std::size_t i = 0; i < live.size(); ++i) {
            const Order& c = live[i].o;
            if (c.side == taker || !crosses(taker, limit, c.price)) continue;
            if (b < 0) { b = static_cast<int>(i); continue; }
            const R& cur = live[static_cast<std::size_t>(b)];
            const bool better = taker == Side::Buy ? c.price < cur.o.price : c.price > cur.o.price;
            const bool tie_earlier = c.price == cur.o.price && live[i].seq < cur.seq;
            if (better || tie_earlier) b = static_cast<int>(i);
        }
        return b;
    }

    // Execute against the book, then rest (GTC) or expire (others). No Accepted/Amended here.
    void run(const Order& taker, Events& out) {
        const Price limit = limit_of(taker);
        Quantity rem = taker.quantity;
        while (rem > 0) {
            const int b = best(taker.side, limit);
            if (b < 0) break;
            R& m = live[static_cast<std::size_t>(b)];
            const Quantity q = std::min(rem, m.o.quantity);
            out.push_back(Trade{m.o.id, taker.id, taker.side, m.o.price, q});
            m.o.quantity -= q;
            rem -= q;
            if (m.o.quantity == 0) live.erase(live.begin() + b);
        }
        if (rem == 0) return;
        if (taker.tif == TimeInForce::GTC) {
            live.push_back({Order(taker.id, taker.side, taker.price, rem), next_seq++});
        } else {
            out.push_back(Expired{taker.id, rem});
        }
    }

    Events submit(const Order& o) {
        Events out;
        if (o.quantity == 0) return {Rejected{o.id, RejectReason::InvalidQuantity}};
        if (o.type == OrderType::Market && o.tif == TimeInForce::GTC) return {Rejected{o.id, RejectReason::InvalidTimeInForce}};
        if (o.type == OrderType::Limit && (o.price < kMinPrice || o.price > kMaxPrice)) return {Rejected{o.id, RejectReason::InvalidPrice}};
        if (find(o.id) >= 0) return {Rejected{o.id, RejectReason::DuplicateId}};
        out.push_back(Accepted{o.id, o.side, o.price, o.quantity, o.tif, o.type});
        if (o.tif == TimeInForce::FOK) {
            Quantity avail = 0;
            for (const R& r : live)
                if (r.o.side != o.side && crosses(o.side, limit_of(o), r.o.price)) avail += r.o.quantity;
            if (avail < o.quantity) {
                out.push_back(Expired{o.id, o.quantity});
                return out;
            }
        }
        run(o, out);
        return out;
    }

    Events cancel(OrderId id) {
        const int i = find(id);
        if (i < 0) return {Rejected{id, RejectReason::UnknownOrder}};
        Events out{Cancelled{id, live[static_cast<std::size_t>(i)].o.quantity}};
        live.erase(live.begin() + i);
        return out;
    }

    Events amend(OrderId id, Price price, Quantity qty) {
        const int i = find(id);
        if (i < 0) return {Rejected{id, RejectReason::UnknownOrder}};
        if (qty == 0) return {Rejected{id, RejectReason::InvalidQuantity}};
        if (price < kMinPrice || price > kMaxPrice) return {Rejected{id, RejectReason::InvalidPrice}};
        R& cur = live[static_cast<std::size_t>(i)];
        const Order old = cur.o;
        if (price == old.price && qty <= old.quantity) {
            cur.o.quantity = qty;
            return {Amended{id, old.price, old.quantity, price, qty, true}};
        }
        live.erase(live.begin() + i);
        Events out{Amended{id, old.price, old.quantity, price, qty, false}};
        run(Order(id, old.side, price, qty), out);
        return out;
    }
};

Events ref_apply(Ref& r, const Command& c) {
    if (const auto* n = std::get_if<NewCommand>(&c)) return r.submit(n->order);
    if (const auto* x = std::get_if<CancelCommand>(&c)) return r.cancel(x->id);
    const auto& a = std::get<AmendCommand>(c);
    return r.amend(a.id, a.price, a.quantity);
}

GeneratorConfig ext_config(std::uint64_t seed, Price band) {
    GeneratorConfig cfg;
    cfg.seed = seed;
    cfg.count = 2500;
    cfg.band = band;
    cfg.bad_percent = 5;
    cfg.sweep_percent = 8;
    cfg.ioc_percent = 15;
    cfg.fok_percent = 12;
    cfg.market_percent = 10;
    cfg.amend_percent = 18;
    return cfg;
}

template <class E>
void run_against_model(std::uint64_t seed, Price band) {
    const auto cmds = generate(ext_config(seed, band));
    E eng;
    Ref ref;
    Events got;
    std::size_t expired = 0, amended = 0, market = 0, lost = 0;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        got.clear();
        if (const auto* n = std::get_if<NewCommand>(&cmds[i])) eng.submit(n->order, got);
        else if (const auto* x = std::get_if<CancelCommand>(&cmds[i])) eng.cancel(x->id, got);
        else eng.amend(std::get<AmendCommand>(cmds[i]).id, std::get<AmendCommand>(cmds[i]).price, std::get<AmendCommand>(cmds[i]).quantity, got);
        const Events want = ref_apply(ref, cmds[i]);
        ASSERT_EQ(got, want) << "seed " << seed << " band " << band << " command " << i;
        std::string why;
        ASSERT_TRUE(eng.book().validate(&why)) << why;
        ASSERT_FALSE(eng.book().is_crossed());
        ASSERT_EQ(eng.book().order_count(), ref.live.size());
        for (const Event& e : got) {
            if (std::holds_alternative<Expired>(e)) ++expired;
            if (const auto* m = std::get_if<Amended>(&e)) { ++amended; lost += !m->keeps_priority; }
            if (const auto* a = std::get_if<Accepted>(&e)) { market += a->type == OrderType::Market; }
        }
    }
    // Make sure the test actually exercised the features it claims to.
    EXPECT_GT(expired, 50u) << "seed " << seed;
    EXPECT_GT(amended, 50u) << "seed " << seed;
    EXPECT_GT(lost, 10u) << "seed " << seed;
    EXPECT_GT(market, 50u) << "seed " << seed;
}

}  // namespace

TYPED_TEST(ExtTyped, RandomSequencesWithAllExtensionsMatchTheNaiveModel) {
    for (std::uint64_t seed : {1u, 2u, 3u, 4u, 5u}) {
        for (Price band : {2, 6, 30}) run_against_model<TypeParam>(seed, band);
    }
}

TEST(ExtGenerator, ZeroExtensionPercentagesLeaveOldStreamsByteIdentical) {
    // FNV-1a of the file produced by the Phase 5 generator for seed 7, 2000 commands, a snapshot every
    // 100. If the generator draws a random number it did not draw before, this changes.
    GeneratorConfig cfg;
    cfg.seed = 7;
    cfg.count = 2000;
    cfg.snapshot_every = 100;
    std::ostringstream os;
    write_commands(os, generate(cfg));
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : os.str()) h = (h ^ c) * 0x100000001b3ull;
    EXPECT_EQ(h, 0x4490111687ab3748ull);
}
