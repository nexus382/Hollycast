// Portions Copyright 2026 The Hollycast Authors
#include "rend/TexCache.h"
#include <gtest/gtest.h>

namespace
{
class TestTexture
{
public:
	TestTexture(TSP tsp, TCW tcw, int area) : tsp(tsp), tcw(tcw), area(area), dirty(FrameCount) {}
	bool Delete() { return deleteAllowed; }

	TSP tsp;
	TCW tcw;
	int area;
	u32 dirty;
	bool deleteAllowed = true;
};

class TestTextureCache final : public BaseTextureCache<TestTexture>
{
public:
	TestTexture *insert(u32 address)
	{
		TCW tcw{};
		tcw.TexAddr = address;
		tcw.PixelFmt = Pixel565;
		return getTextureCacheData(TSP{}, tcw, 0);
	}

	size_t size() const { return cache.size(); }
	size_t cleanupQueueSize() const { return cleanupOrder.size(); }
	void erase(u32 address)
	{
		for (auto it = cache.begin(); it != cache.end(); ++it)
		{
			if (it->second.tcw.TexAddr == address)
			{
				cache.erase(it);
				return;
			}
		}
	}
};

class FrameCountScope
{
public:
	FrameCountScope() : previous(FrameCount) { FrameCount = 1000; }
	~FrameCountScope() { FrameCount = previous; }

private:
	u32 previous;
};
}

TEST(TexCacheEvictionTest, ReclaimsStaleEntriesAcrossRehashAndKeepsFreshEntries)
{
	FrameCountScope frameCount;
	TestTextureCache cache;
	for (u32 address = 0; address < 700; ++address)
	{
		TestTexture *texture = cache.insert(address);
		if (address == 699)
			texture->dirty = 1;
	}

	cache.CollectCleanup();
	EXPECT_EQ(cache.size(), 700u);
	cache.erase(3); // Leave a stale queue key for cleanup to skip.
	for (u32 address = 700; address < 1700; ++address)
		cache.insert(address); // Rehash after scan progress has started.

	for (int frame = 0; frame < 5 && cache.size() == 1699; ++frame)
		cache.CollectCleanup();
	for (int frame = 0; frame < 10; ++frame)
		cache.CollectCleanup();

	EXPECT_EQ(cache.size(), 1698u);
	EXPECT_EQ(cache.cleanupQueueSize(), cache.size());
	for (u32 address = 0; address < 1700; ++address)
	{
		if (address == 3 || address == 699)
			continue;
		TestTexture *texture = cache.insert(address);
		EXPECT_EQ(texture->dirty, 1000u) << "fresh entry " << address;
	}

	cache.Clear();
	EXPECT_EQ(cache.size(), 0u);
	EXPECT_EQ(cache.cleanupQueueSize(), 0u);
}

TEST(TexCacheEvictionTest, KeepsEntryQueuedWhenBackendDeclinesDeletion)
{
	FrameCountScope frameCount;
	TestTextureCache cache;
	TestTexture *texture = cache.insert(1);
	texture->dirty = 1;
	texture->deleteAllowed = false;

	cache.CollectCleanup();
	EXPECT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache.cleanupQueueSize(), 1u);

	texture->deleteAllowed = true;
	cache.CollectCleanup();
	EXPECT_EQ(cache.size(), 0u);
	EXPECT_EQ(cache.cleanupQueueSize(), 0u);
}

TEST(TexCacheEvictionTest, DeletesAtMostSixCandidatesPerCall)
{
	FrameCountScope frameCount;
	TestTextureCache cache;
	for (u32 address = 0; address < 20; ++address)
		cache.insert(address)->dirty = 1;

	cache.CollectCleanup();
	EXPECT_EQ(cache.size(), 14u);
	EXPECT_EQ(cache.cleanupQueueSize(), cache.size());
}

TEST(TexCacheEvictionTest, BackendWithOwnCleanupDoesNotTrackSharedKeys)
{
	class BackendCache : public BaseTextureCache<TestTexture, false>
	{
	public:
		bool trackedForSharedCleanup() const { return !cleanupOrder.empty(); }
	};
	BackendCache cache;
	TCW tcw{};
	tcw.PixelFmt = Pixel565;
	cache.getTextureCacheData(TSP{}, tcw, 0);
	EXPECT_FALSE(cache.trackedForSharedCleanup());
	cache.Clear();
}
