#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "lob/price_level.hpp"

using namespace lob;

namespace {
Order make(OrderId id, Quantity q) { return Order{id, Side::Buy, 100, q}; }

std::vector<OrderId> ids(const PriceLevel& l) {
    std::vector<OrderId> out;
    for (const Order& o : l.orders()) out.push_back(o.id);
    return out;
}
}  // namespace

TEST(PriceLevel, StartsEmpty) {
    PriceLevel l(100);
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.price(), 100);
    EXPECT_EQ(l.total_quantity(), 0u);
    EXPECT_EQ(l.order_count(), 0u);
}

TEST(PriceLevel, AppendKeepsFifoAndTotals) {
    PriceLevel l(100);
    l.append(make(1, 5));
    l.append(make(2, 7));
    l.append(make(3, 1));
    EXPECT_EQ(ids(l), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(l.total_quantity(), 13u);
    EXPECT_EQ(l.order_count(), 3u);
}

TEST(PriceLevel, PopFrontRemovesOldest) {
    PriceLevel l(100);
    l.append(make(1, 5));
    l.append(make(2, 7));
    EXPECT_EQ(l.pop_front().id, 1u);
    EXPECT_EQ(ids(l), (std::vector<OrderId>{2}));
    EXPECT_EQ(l.total_quantity(), 7u);
}

TEST(PriceLevel, EraseByHandleFromMiddleKeepsOthersValid) {
    PriceLevel l(100);
    auto h1 = l.append(make(1, 5));
    auto h2 = l.append(make(2, 7));
    auto h3 = l.append(make(3, 1));
    EXPECT_EQ(l.erase(h2).id, 2u);
    EXPECT_EQ(ids(l), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(l.total_quantity(), 6u);
    // Handles of surviving orders must still be usable (stability).
    EXPECT_EQ(h1->id, 1u);
    EXPECT_EQ(h3->id, 3u);
    l.erase(h1);
    l.erase(h3);
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.total_quantity(), 0u);
}

TEST(PriceLevel, CanAcceptDetectsOverflow) {
    PriceLevel l(100);
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    l.append(make(1, kMax - 10));
    EXPECT_TRUE(l.can_accept(10));
    EXPECT_FALSE(l.can_accept(11));
}

TEST(PriceLevel, ReduceFrontPartialKeepsPositionAndUpdatesTotal) {
    PriceLevel l(100);
    l.append(make(1, 10));
    l.append(make(2, 7));
    EXPECT_FALSE(l.reduce_front(4));
    EXPECT_EQ(ids(l), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(l.orders().front().quantity, 6u);
    EXPECT_EQ(l.total_quantity(), 13u);
}

TEST(PriceLevel, ReduceFrontToZeroRemovesOrder) {
    PriceLevel l(100);
    l.append(make(1, 10));
    l.append(make(2, 7));
    EXPECT_TRUE(l.reduce_front(10));
    EXPECT_EQ(ids(l), (std::vector<OrderId>{2}));
    EXPECT_EQ(l.total_quantity(), 7u);
}
