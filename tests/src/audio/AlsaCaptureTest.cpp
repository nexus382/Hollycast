// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "audio/alsa_capture.h"

#include <cerrno>
#include <cstdint>

namespace {

constexpr uint8_t kGuard = 0xD7;
constexpr uint8_t kUnwritten = 0xA5;

struct GuardedBuffer {
	uint8_t storage[2 + 10 + 2];

	GuardedBuffer()
	{
		for (uint8_t &byte : storage)
			byte = kUnwritten;
		storage[0] = storage[1] = kGuard;
		storage[12] = storage[13] = kGuard;
	}

	void *data() { return storage + 2; }
	uint8_t *bytes() { return storage + 2; }
	void expectGuardsIntact() const
	{
		EXPECT_EQ(storage[0], kGuard);
		EXPECT_EQ(storage[1], kGuard);
		EXPECT_EQ(storage[12], kGuard);
		EXPECT_EQ(storage[13], kGuard);
	}
};

void noopRecovery(int64_t) {}

} // namespace

TEST(AlsaCaptureTest, ShortReadKeepsCapturedPrefix)
{
	GuardedBuffer buffer;
	const uint8_t captured[] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
	for (size_t i = 0; i < sizeof(captured); i++)
		buffer.bytes()[i] = captured[i];

	const uint32_t frames = audio::alsa::finishMonoCapture(buffer.data(), 5, int64_t{3},
			int64_t{-EAGAIN}, noopRecovery);

	EXPECT_EQ(frames, 3u);
	for (size_t i = 0; i < sizeof(captured); i++)
		EXPECT_EQ(buffer.bytes()[i], captured[i]);
	for (size_t i = sizeof(captured); i < 10; i++)
		EXPECT_EQ(buffer.bytes()[i], 0);
	buffer.expectGuardsIntact();
}

TEST(AlsaCaptureTest, PartialOddFullAndOversizedReadsAreBounded)
{
	struct Case {
		int64_t readFrames;
		uint32_t expectedFrames;
	};
	const Case cases[] = {
		{ 0, 0 }, { 1, 1 }, { 3, 3 }, { 5, 5 }, { 8, 5 },
	};

	for (const Case &testCase : cases) {
		GuardedBuffer buffer;
		const uint32_t frames = audio::alsa::finishMonoCapture(buffer.data(), 5,
				testCase.readFrames, int64_t{-EAGAIN}, noopRecovery);
		EXPECT_EQ(frames, testCase.expectedFrames);
		for (size_t i = 0; i < frames * sizeof(int16_t); i++)
			EXPECT_EQ(buffer.bytes()[i], kUnwritten);
		for (size_t i = frames * sizeof(int16_t); i < 10; i++)
			EXPECT_EQ(buffer.bytes()[i], 0);
		buffer.expectGuardsIntact();
	}
}

TEST(AlsaCaptureTest, NoDataAndRecoveryDoNotBecomeFrameCounts)
{
	GuardedBuffer noDataBuffer;
	int recoveries = 0;
	const uint32_t noDataFrames = audio::alsa::finishMonoCapture(noDataBuffer.data(), 5,
			int64_t{-EAGAIN}, int64_t{-EAGAIN}, [&](int64_t) { recoveries++; });
	EXPECT_EQ(noDataFrames, 0u);
	EXPECT_EQ(recoveries, 0);
	for (size_t i = 0; i < 10; i++)
		EXPECT_EQ(noDataBuffer.bytes()[i], 0);
	noDataBuffer.expectGuardsIntact();

	for (int recoveryResult : { 0, -EIO }) {
		GuardedBuffer failedReadBuffer;
		int recoveryCalls = 0;
		const uint32_t failedReadFrames = audio::alsa::finishMonoCapture(
				failedReadBuffer.data(), 5, int64_t{-EIO}, int64_t{-EAGAIN},
				[&](int64_t readError) {
					EXPECT_EQ(readError, -EIO);
					recoveryCalls++;
					return recoveryResult;
				});
		EXPECT_EQ(failedReadFrames, 0u);
		EXPECT_EQ(recoveryCalls, 1);
		for (size_t i = 0; i < 10; i++)
			EXPECT_EQ(failedReadBuffer.bytes()[i], 0);
		failedReadBuffer.expectGuardsIntact();
	}

	GuardedBuffer nextReadBuffer;
	nextReadBuffer.bytes()[0] = 0x31;
	nextReadBuffer.bytes()[1] = 0x42;
	const uint32_t nextReadFrames = audio::alsa::finishMonoCapture(nextReadBuffer.data(), 5,
			int64_t{1}, int64_t{-EAGAIN}, noopRecovery);
	EXPECT_EQ(nextReadFrames, 1u);
	EXPECT_EQ(nextReadBuffer.bytes()[0], 0x31);
	EXPECT_EQ(nextReadBuffer.bytes()[1], 0x42);
	for (size_t i = 2; i < 10; i++)
		EXPECT_EQ(nextReadBuffer.bytes()[i], 0);
	nextReadBuffer.expectGuardsIntact();
}

TEST(AlsaCaptureTest, ZeroCapacityDoesNotTouchBuffer)
{
	int recoveryCalls = 0;
	const uint32_t frames = audio::alsa::finishMonoCapture(nullptr, 0, int64_t{-EIO},
			int64_t{-EAGAIN}, [&](int64_t) { recoveryCalls++; });
	EXPECT_EQ(frames, 0u);
	EXPECT_EQ(recoveryCalls, 1);
}
