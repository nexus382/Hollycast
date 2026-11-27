#include "gtest/gtest.h"
#include "ui/boxart/pvrparser.h"

namespace {
std::vector<u8> makePvr(u16 width, u16 height)
{
	const size_t pixelBytes = static_cast<size_t>(width) * height * 2;
	std::vector<u8> data(16 + pixelBytes, 0);
	memcpy(data.data(), "PVRT", 4);
	const u32 chunkSize = static_cast<u32>(12 + pixelBytes);
	memcpy(data.data() + 4, &chunkSize, sizeof(chunkSize));
	data[8] = Pixel565;
	data[9] = PvrSquareTwiddled;
	memcpy(data.data() + 12, &width, sizeof(width));
	memcpy(data.data() + 14, &height, sizeof(height));
	return data;
}
}

TEST(PvrParserTest, ParsesMinimalValidTexture)
{
	std::vector<u8> data = makePvr(8, 8);
	std::vector<u8> pixels;
	u32 width = 0;
	u32 height = 0;

	ASSERT_TRUE(pvrParse(data.data(), data.size(), width, height, pixels));
	EXPECT_EQ(8u, width);
	EXPECT_EQ(8u, height);
	EXPECT_EQ(8u * 8 * 4, pixels.size());
}

TEST(PvrParserTest, RejectsZeroDimensions)
{
	std::vector<u8> data = makePvr(0, 8);
	std::vector<u8> pixels;
	u32 width = 0;
	u32 height = 0;

	EXPECT_FALSE(pvrParse(data.data(), data.size(), width, height, pixels));
}

TEST(PvrParserTest, RejectsDimensionsOutsideSupportedRange)
{
	std::vector<u8> data = makePvr(65535, 8);
	std::vector<u8> pixels;
	u32 width = 0;
	u32 height = 0;

	EXPECT_FALSE(pvrParse(data.data(), 16, width, height, pixels));
}
