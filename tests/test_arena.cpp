#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "lob/arena.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;

TEST(Arena, BlocksAreAlignedDistinctAndKeepTheirContents) {
    SlabArena a;
    struct Block {
        unsigned char* p;
        std::size_t n;
        unsigned char fill;
    };
    std::vector<Block> blocks;
    const std::size_t sizes[] = {1, 8, 16, 17, 40, 48, 100, 255, 256};
    for (int round = 0; round < 3000; ++round) {
        const std::size_t n = sizes[round % 9];
        auto* p = static_cast<unsigned char*>(a.allocate(n, 8));
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(p) % SlabArena::kGranule, 0u);
        const auto fill = static_cast<unsigned char>(round);
        std::memset(p, fill, n);
        blocks.push_back({p, n, fill});
    }
    // No block was clobbered by a neighbour => no overlap.
    for (const Block& b : blocks)
        for (std::size_t i = 0; i < b.n; ++i) ASSERT_EQ(b.p[i], b.fill);
    std::set<void*> uniq;
    for (const Block& b : blocks) uniq.insert(b.p);
    EXPECT_EQ(uniq.size(), blocks.size());
    for (const Block& b : blocks) a.deallocate(b.p, b.n, 8);
}

TEST(Arena, FreedBlocksAreReusedLifoWithinASizeClass) {
    SlabArena a;
    void* p1 = a.allocate(48, 8);
    void* p2 = a.allocate(48, 8);
    a.deallocate(p1, 48, 8);
    a.deallocate(p2, 48, 8);
    EXPECT_EQ(a.allocate(48, 8), p2);  // last freed, first reused (cache-warm)
    EXPECT_EQ(a.allocate(48, 8), p1);
    const std::size_t chunks = a.chunk_count();
    EXPECT_EQ(chunks, 1u);
}

TEST(Arena, SizesRoundUpToTheSameClassShareBlocks) {
    SlabArena a;
    void* p = a.allocate(33, 8);  // class 48
    a.deallocate(p, 33, 8);
    EXPECT_EQ(a.allocate(48, 8), p);
    EXPECT_EQ(a.chunk_count(), 1u);
}

TEST(Arena, SteadyChurnDoesNotGrowMemory) {
    SlabArena a;
    std::vector<void*> live;
    for (int i = 0; i < 1000; ++i) live.push_back(a.allocate(48, 8));
    const std::size_t reserved = a.bytes_reserved();
    for (int round = 0; round < 100; ++round) {
        for (void* p : live) a.deallocate(p, 48, 8);
        for (void*& p : live) p = a.allocate(48, 8);
    }
    EXPECT_EQ(a.bytes_reserved(), reserved);  // all churn served from free lists
}

TEST(Arena, GrowsByWholeChunksAndNothingIsLost) {
    SlabArena a;
    const std::size_t per_chunk = (SlabArena::kChunkBytes - 16) / 48;
    for (std::size_t i = 0; i < per_chunk; ++i) a.allocate(48, 8);
    EXPECT_EQ(a.chunk_count(), 1u);
    a.allocate(48, 8);
    EXPECT_EQ(a.chunk_count(), 2u);
    EXPECT_EQ(a.bytes_reserved(), 2 * SlabArena::kChunkBytes);
}

TEST(Arena, LargeAndOverAlignedRequestsFallThroughToTheSystemAllocator) {
    SlabArena a;
    void* big = a.allocate(10'000, 8);
    ASSERT_NE(big, nullptr);
    std::memset(big, 0xAB, 10'000);
    a.deallocate(big, 10'000, 8);
    void* aligned = a.allocate(64, 64);  // small but over-aligned
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 64, 0u);
    a.deallocate(aligned, 64, 64);
    EXPECT_EQ(a.chunk_count(), 0u);  // neither touched the pooled chunks
}

TEST(Arena, WorksUnderStandardContainers) {
    SlabArena a;
    std::list<int, ArenaAllocator<int>> l{ArenaAllocator<int>(&a)};
    for (int i = 0; i < 5000; ++i) l.push_back(i);
    for (int i = 0; i < 5000; i += 2) l.remove(i);
    EXPECT_EQ(l.size(), 2500u);

    std::map<int, int, std::less<int>, ArenaAllocator<std::pair<const int, int>>> m{
        std::less<int>{}, ArenaAllocator<std::pair<const int, int>>(&a)};
    for (int i = 0; i < 2000; ++i) m[i] = i * 2;
    EXPECT_EQ(m.size(), 2000u);
    EXPECT_EQ(m.at(1999), 3998);

    std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, ArenaAllocator<std::pair<const int, int>>> u{
        0, std::hash<int>{}, std::equal_to<int>{}, ArenaAllocator<std::pair<const int, int>>(&a)};
    for (int i = 0; i < 5000; ++i) u[i] = i;  // forces several rehashes (big bucket arrays)
    for (int i = 0; i < 5000; ++i) ASSERT_EQ(u.at(i), i);
}

TEST(Arena, AllocatorEqualityFollowsTheArena) {
    SlabArena a, b;
    EXPECT_TRUE(ArenaAllocator<int>(&a) == ArenaAllocator<long>(&a));
    EXPECT_FALSE(ArenaAllocator<int>(&a) == ArenaAllocator<int>(&b));
}

// A book holds list iterators into its own nodes (and arena pointers); copying or
// moving one would alias or dangle. Earlier phases allowed the copy; this pins the fix.
TEST(Books, AreNotCopyableOrMovable) {
    static_assert(!std::is_copy_constructible_v<OrderBook>);
    static_assert(!std::is_copy_assignable_v<OrderBook>);
    static_assert(!std::is_move_constructible_v<OrderBook>);
    static_assert(!std::is_copy_constructible_v<PooledOrderBook>);
    static_assert(!std::is_move_constructible_v<PooledOrderBook>);
    static_assert(!std::is_copy_constructible_v<MatchingEngine>);
    static_assert(!std::is_copy_constructible_v<PooledMatchingEngine>);
    SUCCEED();
}
