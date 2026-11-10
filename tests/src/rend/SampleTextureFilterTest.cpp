// Portions Copyright 2026 The Hollycast Authors
#include "rend/sample_filter.h"
#include <gtest/gtest.h>
#include <climits>
#include <set>

TEST(SampleTextureFilter, InvalidAndRetiredModesDisableFiltering)
{
	for (int mode : {INT_MIN, -1, 0, 14, 20, INT_MAX})
	{
		const sample_filter::Settings filter(mode, 256);
		EXPECT_EQ(filter.mode, 0);
		EXPECT_EQ(filter.maxSize, 0);
		EXPECT_EQ(filter.key(), 0u);
		EXPECT_FALSE(filter.appliesTo(128));
	}
	for (int size : {INT_MIN, 0, 7})
		EXPECT_EQ(sample_filter::Settings(1, size).key(), 0u);
}

TEST(SampleTextureFilter, SizeLimitsMatchUploadedTextures)
{
	const sample_filter::Settings filter(1, 256);
	EXPECT_FALSE(filter.appliesTo(0));
	EXPECT_TRUE(filter.appliesTo(128));
	EXPECT_TRUE(filter.appliesTo(256));
	// A native 128px texture upscaled to 512px must use the shader's 512px width.
	EXPECT_FALSE(filter.appliesTo(512));
	EXPECT_TRUE(sample_filter::Settings(1, 1024).appliesTo(4096));
	EXPECT_EQ(sample_filter::Settings(1, 300).maxSize, 256);
	EXPECT_EQ(sample_filter::Settings(1, INT_MAX).maxSize, 1024);
}

TEST(SampleTextureFilter, EveryModeAndSizeHasADistinctKey)
{
	std::set<unsigned> keys{0};
	for (int mode = 1; mode <= 19; ++mode)
	{
		if (mode == 14)
			continue;
		for (int size : {8, 16, 32, 64, 128, 256, 512, 1024})
		{
			const sample_filter::Settings filter(mode, size);
			EXPECT_EQ(filter.mode, mode);
			EXPECT_EQ(filter.maxSize, size);
			EXPECT_LT(filter.key(), 1u << 9);
			EXPECT_TRUE(keys.insert(filter.key()).second) << mode << ":" << size;
		}
	}
	EXPECT_EQ(keys.size(), 145u);
}
