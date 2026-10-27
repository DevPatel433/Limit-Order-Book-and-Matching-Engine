#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

// Every test in this file runs against both the baseline and the pooled book.
template <class B>
class OrderBookTyped : public ::testing::Test {};
using BothBooks = ::testing::Types<OrderBook, PooledOrderBook>;
TYPED_TEST_SUITE(OrderBookTyped, BothBooks);

namespace {

Order buy(OrderId id, Price p, Quantity q) { return Order{id, Side::Buy, p, q}; }
Order sell(OrderId id, Price p, Quantity q) { return Order{id, Side::Sell, p, q}; }

template <class B>
void ExpectValid(const B& b) {
    std::string why;
    EXPECT_TRUE(b.validate(&why)) << why;
}

template <class B>
std::vector<OrderId> queue_ids(const B& b, Side s, Price p) {
    std::vector<OrderId> out;
    if (const auto* l = b.level(s, p))
        for (const Order& o : l->orders()) out.push_back(o.id);
    return out;
}

}  // namespace

// ------------------------------------------------------------- empty / sides --

TYPED_TEST(OrderBookTyped, EmptyBook) {
    TypeParam b;
    EXPECT_FALSE(b.best_bid());
    EXPECT_FALSE(b.best_ask());
    EXPECT_FALSE(b.spread());
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.level_count(Side::Buy), 0u);
    EXPECT_EQ(b.level_count(Side::Sell), 0u);
    EXPECT_EQ(b.quantity_at(Side::Buy, 100), 0u);
    EXPECT_EQ(b.level(Side::Sell, 100), nullptr);
    EXPECT_FALSE(b.find(1));
    const BookSnapshot s = b.snapshot();
    EXPECT_TRUE(s.bids.empty());
    EXPECT_TRUE(s.asks.empty());
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, BidsOnly) {
    TypeParam b;
    ASSERT_EQ(b.add(buy(1, 100, 10)), AddStatus::Ok);
    ASSERT_EQ(b.add(buy(2, 101, 5)), AddStatus::Ok);
    EXPECT_EQ(b.best_bid(), 101);
    EXPECT_FALSE(b.best_ask());
    EXPECT_FALSE(b.spread());
    EXPECT_TRUE(b.snapshot().asks.empty());
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, AsksOnly) {
    TypeParam b;
    ASSERT_EQ(b.add(sell(1, 105, 10)), AddStatus::Ok);
    ASSERT_EQ(b.add(sell(2, 104, 5)), AddStatus::Ok);
    EXPECT_FALSE(b.best_bid());
    EXPECT_EQ(b.best_ask(), 104);
    EXPECT_FALSE(b.spread());
    EXPECT_TRUE(b.snapshot().bids.empty());
    ExpectValid(b);
}

// ------------------------------------------------------------ multiple levels --

TYPED_TEST(OrderBookTyped, SnapshotsAreSortedBestFirst) {
    TypeParam b;
    // Insert deliberately out of order.
    for (auto [id, p] : std::vector<std::pair<OrderId, Price>>{{1, 99}, {2, 101}, {3, 100}, {4, 98}})
        ASSERT_EQ(b.add(buy(id, p, 10)), AddStatus::Ok);
    for (auto [id, p] : std::vector<std::pair<OrderId, Price>>{{11, 104}, {12, 102}, {13, 103}, {14, 105}})
        ASSERT_EQ(b.add(sell(id, p, 10)), AddStatus::Ok);

    const BookSnapshot s = b.snapshot();
    std::vector<Price> bid_prices, ask_prices;
    for (auto& l : s.bids) bid_prices.push_back(l.price);
    for (auto& l : s.asks) ask_prices.push_back(l.price);
    EXPECT_EQ(bid_prices, (std::vector<Price>{101, 100, 99, 98}));
    EXPECT_EQ(ask_prices, (std::vector<Price>{102, 103, 104, 105}));
    EXPECT_EQ(b.best_bid(), 101);
    EXPECT_EQ(b.best_ask(), 102);
    EXPECT_EQ(b.spread(), 1);
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, DepthLimitsLevelsPerSide) {
    TypeParam b;
    for (Price p = 100; p < 110; ++p) {
        b.add(buy(static_cast<OrderId>(p), p - 50, 1));
        b.add(sell(static_cast<OrderId>(1000 + p), p + 50, 1));
    }
    EXPECT_EQ(b.snapshot(3).bids.size(), 3u);
    EXPECT_EQ(b.snapshot(3).asks.size(), 3u);
    EXPECT_EQ(b.snapshot(0).bids.size(), 0u);
    EXPECT_EQ(b.snapshot(100).bids.size(), 10u);
    // Top 3 are the *best* 3.
    EXPECT_EQ(b.snapshot(3).bids.front().price, 59);
    EXPECT_EQ(b.snapshot(3).asks.front().price, 150);
}

TYPED_TEST(OrderBookTyped, LevelAggregatesQuantityAndCount) {
    TypeParam b;
    b.add(buy(1, 100, 10));
    b.add(buy(2, 100, 20));
    b.add(buy(3, 99, 7));
    const BookSnapshot s = b.snapshot();
    ASSERT_EQ(s.bids.size(), 2u);
    EXPECT_EQ(s.bids[0], (LevelSnapshot{100, 30, 2}));
    EXPECT_EQ(s.bids[1], (LevelSnapshot{99, 7, 1}));
    EXPECT_EQ(b.quantity_at(Side::Buy, 100), 30u);
}

TYPED_TEST(OrderBookTyped, SameIdOnlyConflictsWithinLiveOrders_NoCrossSideLeak) {
    TypeParam b;
    b.add(buy(1, 100, 10));
    b.add(sell(2, 100, 10));  // same price on opposite sides lives on a different side
    EXPECT_EQ(b.quantity_at(Side::Buy, 100), 10u);
    EXPECT_EQ(b.quantity_at(Side::Sell, 100), 10u);
    ExpectValid(b);
}

// ------------------------------------------------------------------- FIFO --

TYPED_TEST(OrderBookTyped, SamePriceIsFifo) {
    TypeParam b;
    for (OrderId id : {5, 3, 9, 1}) ASSERT_EQ(b.add(buy(id, 100, 1)), AddStatus::Ok);
    EXPECT_EQ(queue_ids(b, Side::Buy, 100), (std::vector<OrderId>{5, 3, 9, 1}));  // arrival, not id order
    for (OrderId id : {20, 10, 30}) ASSERT_EQ(b.add(sell(id, 105, 1)), AddStatus::Ok);
    EXPECT_EQ(queue_ids(b, Side::Sell, 105), (std::vector<OrderId>{20, 10, 30}));
}

TYPED_TEST(OrderBookTyped, CancelPreservesFifoOfRemaining) {
    TypeParam b;
    for (OrderId id : {1, 2, 3, 4}) b.add(buy(id, 100, 10));
    ASSERT_TRUE(b.cancel(2));
    EXPECT_EQ(queue_ids(b, Side::Buy, 100), (std::vector<OrderId>{1, 3, 4}));
    b.add(buy(5, 100, 10));
    EXPECT_EQ(queue_ids(b, Side::Buy, 100), (std::vector<OrderId>{1, 3, 4, 5}));
    EXPECT_EQ(b.quantity_at(Side::Buy, 100), 40u);
    ExpectValid(b);
}

// ----------------------------------------------------------------- cancel --

TYPED_TEST(OrderBookTyped, CancelLastOrderRemovesLevelAndUpdatesBest) {
    TypeParam b;
    b.add(buy(1, 101, 10));
    b.add(buy(2, 100, 10));
    ASSERT_TRUE(b.cancel(1));
    EXPECT_EQ(b.best_bid(), 100);
    EXPECT_EQ(b.level_count(Side::Buy), 1u);
    EXPECT_EQ(b.level(Side::Buy, 101), nullptr);
    ASSERT_TRUE(b.cancel(2));
    EXPECT_FALSE(b.best_bid());
    EXPECT_EQ(b.order_count(), 0u);
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, CancelUnknownOrAlreadyCancelledReturnsFalse) {
    TypeParam b;
    EXPECT_FALSE(b.cancel(42));
    b.add(buy(1, 100, 10));
    EXPECT_TRUE(b.cancel(1));
    EXPECT_FALSE(b.cancel(1));
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, IdCanBeReusedAfterCancel) {
    TypeParam b;
    ASSERT_EQ(b.add(buy(1, 100, 10)), AddStatus::Ok);
    ASSERT_TRUE(b.cancel(1));
    ASSERT_EQ(b.add(sell(1, 110, 3)), AddStatus::Ok);
    EXPECT_EQ(b.find(1), (Order{1, Side::Sell, 110, 3}));
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, FindReturnsStoredOrder) {
    TypeParam b;
    b.add(buy(7, 100, 10));
    EXPECT_EQ(b.find(7), (Order{7, Side::Buy, 100, 10}));
    EXPECT_TRUE(b.contains(7));
    EXPECT_FALSE(b.contains(8));
}

// ------------------------------------------------------------- validation --

TYPED_TEST(OrderBookTyped, RejectsZeroQuantity) {
    TypeParam b;
    EXPECT_EQ(b.add(buy(1, 100, 0)), AddStatus::InvalidQuantity);
    EXPECT_EQ(b.order_count(), 0u);
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, RejectsOutOfRangePrices) {
    TypeParam b;
    EXPECT_EQ(b.add(buy(1, 0, 1)), AddStatus::InvalidPrice);
    EXPECT_EQ(b.add(buy(2, -5, 1)), AddStatus::InvalidPrice);
    EXPECT_EQ(b.add(sell(3, kMaxPrice + 1, 1)), AddStatus::InvalidPrice);
    EXPECT_EQ(b.add(sell(4, std::numeric_limits<Price>::max(), 1)), AddStatus::InvalidPrice);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.add(buy(5, kMinPrice, 1)), AddStatus::Ok);
    EXPECT_EQ(b.add(sell(6, kMaxPrice, 1)), AddStatus::Ok);
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, SpreadCannotOverflowAtPriceExtremes) {
    TypeParam b;
    b.add(buy(1, kMinPrice, 1));
    b.add(sell(2, kMaxPrice, 1));
    EXPECT_EQ(b.spread(), kMaxPrice - kMinPrice);
}

TYPED_TEST(OrderBookTyped, DuplicateIdRejectedAndBookUnchanged) {
    TypeParam b;
    ASSERT_EQ(b.add(buy(1, 100, 10)), AddStatus::Ok);
    const BookSnapshot before = b.snapshot();
    EXPECT_EQ(b.add(buy(1, 100, 5)), AddStatus::DuplicateId);
    EXPECT_EQ(b.add(sell(1, 200, 5)), AddStatus::DuplicateId);  // even on the other side
    EXPECT_EQ(b.snapshot(), before);
    EXPECT_EQ(b.find(1), (Order{1, Side::Buy, 100, 10}));
    ExpectValid(b);
}

TYPED_TEST(OrderBookTyped, LevelQuantityOverflowRejectedAndBookUnchanged) {
    TypeParam b;
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    ASSERT_EQ(b.add(buy(1, 100, kMax - 5)), AddStatus::Ok);
    EXPECT_EQ(b.add(buy(2, 100, 6)), AddStatus::QuantityOverflow);
    EXPECT_FALSE(b.contains(2));
    EXPECT_EQ(b.quantity_at(Side::Buy, 100), kMax - 5);
    EXPECT_EQ(b.add(buy(3, 100, 5)), AddStatus::Ok);  // exactly fits
    EXPECT_EQ(b.quantity_at(Side::Buy, 100), kMax);
    EXPECT_EQ(b.add(buy(4, 99, 6)), AddStatus::Ok);   // other levels unaffected
    ExpectValid(b);
}

// --------------------------------------------------- Phase 1 scope boundary --

TYPED_TEST(OrderBookTyped, Phase1DoesNotMatch_CrossedBookIsStoredAsIs) {
    TypeParam b;
    b.add(buy(1, 101, 10));
    b.add(sell(2, 100, 10));
    EXPECT_EQ(b.best_bid(), 101);
    EXPECT_EQ(b.best_ask(), 100);
    EXPECT_EQ(b.spread(), -1);  // Phase 2's engine will make this state unreachable
    EXPECT_EQ(b.order_count(), 2u);
    ExpectValid(b);
}

// ------------------------------------------------------------ determinism --

namespace {
struct Op {
    bool is_add;
    Order order;
    OrderId cancel_id;
};

std::vector<Op> random_ops(std::uint32_t seed, int n) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    OrderId next_id = 1;
    for (int i = 0; i < n; ++i) {
        if (next_id > 1 && rng() % 100 < 35) {
            // Cancel something that may or may not be live (including unknown IDs).
            ops.push_back({false, {}, 1 + rng() % (next_id + 3)});
        } else {
            Order o{next_id++, (rng() & 1) ? Side::Buy : Side::Sell,
                    static_cast<Price>(90 + rng() % 21), 1 + rng() % 50};
            ops.push_back({true, o, 0});
        }
    }
    return ops;
}

// Independent, obviously-correct model: a flat map of live orders.
BookSnapshot model_snapshot(const std::map<OrderId, Order>& live) {
    std::map<Price, LevelSnapshot> bids, asks;
    for (const auto& [id, o] : live) {
        auto& side = o.side == Side::Buy ? bids : asks;
        auto& l = side[o.price];
        l.price = o.price;
        l.quantity += o.quantity;
        ++l.order_count;
    }
    BookSnapshot s;
    for (auto it = bids.rbegin(); it != bids.rend(); ++it) s.bids.push_back(it->second);
    for (auto& [p, l] : asks) s.asks.push_back(l);
    return s;
}
}  // namespace

TYPED_TEST(OrderBookTyped, RandomOpsMatchReferenceModelAndKeepInvariants) {
    for (std::uint32_t seed : {1u, 2u, 3u, 12345u}) {
        TypeParam b;
        std::map<OrderId, Order> live;
        for (const Op& op : random_ops(seed, 1500)) {
            if (op.is_add) {
                ASSERT_EQ(b.add(op.order), AddStatus::Ok);
                live.emplace(op.order.id, op.order);
            } else {
                ASSERT_EQ(b.cancel(op.cancel_id), live.erase(op.cancel_id) == 1) << "seed " << seed;
            }
            std::string why;
            ASSERT_TRUE(b.validate(&why)) << "seed " << seed << ": " << why;
        }
        EXPECT_EQ(b.snapshot(), model_snapshot(live)) << "seed " << seed;
        EXPECT_EQ(b.order_count(), live.size());
    }
}

TYPED_TEST(OrderBookTyped, SameInputProducesIdenticalSnapshot) {
    const auto ops = random_ops(777, 1000);
    auto run = [&] {
        TypeParam b;
        for (const Op& op : ops) op.is_add ? (void)b.add(op.order) : (void)b.cancel(op.cancel_id);
        return b.snapshot();
    };
    EXPECT_EQ(run(), run());
}
