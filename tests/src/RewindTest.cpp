// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/flashrom/flashrom.h"
#include "hw/maple/maple_if.h"
#include "hw/naomi/card_reader.h"
#include "input/maplelink.h"
#include "network/net_handshake.h"
#include "rewind.h"
#include "serialize.h"
#include "types.h"

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

class RewindTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		rewind_state::releaseRequests();
		config::RewindEnable = true;
		config::RewindInterval = 200;
		config::RewindDuration = 30;
		config::RewindCompression = 0;
		config::ThreadedRendering = false;
		config::GGPOEnable = false;
		config::BattleCableEnable = false;
		config::SerialConsole = false;
		settings.content.path = "rewind-test.iso";
		settings.content.gameId.clear();
		settings.platform.system = DC_PLATFORM_DREAMCAST;
		settings.platform.ram_size = 16 * 1024 * 1024;
		settings.network.online = false;
		settings.naomi.multiboard = false;
		settings.naomi.slave = false;
		settings.naomi.drivingSimSlave = false;
		settings.raHardcoreMode = false;
		rewind_state::invalidate();
		rewind_state::poll();
	}

	void TearDown() override
	{
		rewind_state::releaseRequests();
		config::RewindEnable = previousRewindEnable;
		config::RewindInterval = previousRewindInterval;
		config::RewindDuration = previousRewindDuration;
		config::RewindCompression = previousRewindCompression;
		config::ThreadedRendering = previousThreadedRendering;
		config::GGPOEnable = previousGgpoEnable;
		config::BattleCableEnable = previousBattleCableEnable;
		config::SerialConsole = previousSerialConsole;
		settings.content.path = previousContentPath;
		settings.content.gameId = previousGameId;
		settings.platform.system = previousPlatform;
		settings.platform.ram_size = previousRamSize;
		settings.network.online = previousOnline;
		settings.naomi.multiboard = previousMultiboard;
		settings.naomi.slave = previousSlave;
		settings.naomi.drivingSimSlave = previousDrivingSimSlave;
		settings.raHardcoreMode = previousHardcore;
		rewind_state::invalidate();
		rewind_state::poll();
	}

private:
	const bool previousRewindEnable = config::RewindEnable.get();
	const int previousRewindInterval = config::RewindInterval.get();
	const int previousRewindDuration = config::RewindDuration.get();
	const int previousRewindCompression = config::RewindCompression.get();
	const bool previousThreadedRendering = config::ThreadedRendering.get();
	const bool previousGgpoEnable = config::GGPOEnable.get();
	const bool previousBattleCableEnable = config::BattleCableEnable.get();
	const bool previousSerialConsole = config::SerialConsole.get();
	const std::string previousContentPath = settings.content.path;
	const std::string previousGameId = settings.content.gameId;
	const int previousPlatform = settings.platform.system;
	const u32 previousRamSize = settings.platform.ram_size;
	const bool previousOnline = settings.network.online;
	const bool previousMultiboard = settings.naomi.multiboard;
	const bool previousSlave = settings.naomi.slave;
	const int previousDrivingSimSlave = settings.naomi.drivingSimSlave;
	const bool previousHardcore = settings.raHardcoreMode;
};

} // namespace

TEST_F(RewindTest, ReleasingOneInputSourceDoesNotReleaseAnother)
{
	int keyboardSource = 0;
	int controllerSource = 0;
	rewind_state::setRequested(&keyboardSource, true);
	rewind_state::setRequested(&controllerSource, true);
	EXPECT_TRUE(rewind_state::requested());

	rewind_state::setRequested(&keyboardSource, false);
	EXPECT_TRUE(rewind_state::requested());
	EXPECT_FALSE(rewind_state::requested(&keyboardSource));
	EXPECT_TRUE(rewind_state::requested(&controllerSource));
	rewind_state::setRequested(&controllerSource, false);
	EXPECT_FALSE(rewind_state::requested());
}

TEST_F(RewindTest, AvailableWhenAllGatesPass)
{
	EXPECT_EQ(nullptr, rewind_state::unavailableReason());
	EXPECT_FALSE(rewind_state::canStep());
}

TEST_F(RewindTest, SharedThreadRequirementIgnoresCurrentThreadMode)
{
	config::ThreadedRendering = true;
	EXPECT_TRUE(rewind_state::requiresSharedThread());
	EXPECT_STREQ("Restart the game to use Rewind with shared emulation and rendering.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, SharedThreadRequirementMatchesRewindPolicy)
{
	EXPECT_TRUE(rewind_state::requiresSharedThread());

	config::RewindEnable = false;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	config::RewindEnable = true;

	settings.content.path.clear();
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.content.path = "rewind-test.iso";

	settings.platform.system = DC_PLATFORM_HIKARU;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.platform.system = DC_PLATFORM_SYSTEMSP;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.platform.system = DC_PLATFORM_DREAMCAST;

	settings.raHardcoreMode = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.raHardcoreMode = false;
	settings.network.online = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.network.online = false;
	config::GGPOEnable = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	config::GGPOEnable = false;
	settings.naomi.multiboard = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.naomi.multiboard = false;
	settings.naomi.slave = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	settings.naomi.slave = false;
	settings.naomi.drivingSimSlave = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
}

TEST_F(RewindTest, MapleLinkDeviceBlocksRewindAndSharedThreadRequirement)
{
	const std::shared_ptr<maple_device> previousDevice = MapleDevices[0][MAPLE_MAIN_DEV_IDX];
	auto linkStub = std::make_shared<MapleLinkStub>();
	// Setup initializes config, which maple_device's destructor expects.
	linkStub->Setup(0, MAPLE_MAIN_DEV_IDX, -1, false);
	MapleDevices[0][MAPLE_MAIN_DEV_IDX] = linkStub;

	EXPECT_FALSE(rewind_state::requiresSharedThread());
	config::ThreadedRendering = true;
	EXPECT_STREQ("Restart the game to use Rewind with shared emulation and rendering.", rewind_state::unavailableReason());
	config::ThreadedRendering = false;
	EXPECT_STREQ("Rewind is unavailable with passthrough DreamLink devices.", rewind_state::unavailableReason());
	int source = 0;
	rewind_state::setRequested(&source, true);
	EXPECT_FALSE(rewind_state::canStep());
	EXPECT_EQ(rewind_state::FrameAction::Run, rewind_state::beforeFrame());

	MapleDevices[0][MAPLE_MAIN_DEV_IDX] = previousDevice;
}

TEST_F(RewindTest, ArcadeCardReadersBlockRewindUntilDetached)
{
	settings.platform.system = DC_PLATFORM_NAOMI;
	card_reader::barcodeInit();
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	EXPECT_NE(nullptr, rewind_state::unavailableReason());
	card_reader::barcodeTerm();
	EXPECT_EQ(nullptr, rewind_state::unavailableReason());

	auto reader = RFIDReaderWriter::Create();
	reader->Setup(0, MAPLE_MAIN_DEV_IDX, 0, false);
	auto& device = MapleDevices[0][MAPLE_MAIN_DEV_IDX];
	device.swap(reader);
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	EXPECT_FALSE(rewind_state::canStep());
	device.swap(reader);
	EXPECT_EQ(nullptr, rewind_state::unavailableReason());
}

TEST_F(RewindTest, HostSerialOutputBlocksRewind)
{
	config::SerialConsole = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	EXPECT_STREQ("Rewind is unavailable with host serial output enabled.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, TitlesWithUnsavedSerialDevicesBlockRewindOffline)
{
	const char* reason = "Rewind is unavailable for this game's serial device.";
	settings.content.gameId = "HDR-0094";
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	EXPECT_STREQ(reason, rewind_state::unavailableReason());

	settings.platform.system = DC_PLATFORM_NAOMI;
	settings.content.gameId = "ALIEN FRONT";
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	EXPECT_STREQ(reason, rewind_state::unavailableReason());

	settings.content.gameId = "CRAZY TAXI";
	EXPECT_TRUE(rewind_state::requiresSharedThread());
	EXPECT_EQ(nullptr, rewind_state::unavailableReason());
}

TEST_F(RewindTest, RestrictedRewindRequestsDoNotRestoreFrames)
{
	int source = 0;
	auto expectBlocked = [&source] {
		rewind_state::setRequested(&source, true);
		EXPECT_FALSE(rewind_state::canStep());
		EXPECT_EQ(rewind_state::FrameAction::Run, rewind_state::beforeFrame());
		EXPECT_FALSE(rewind_state::requested());
	};

	settings.raHardcoreMode = true;
	expectBlocked();
	settings.raHardcoreMode = false;
	settings.network.online = true;
	expectBlocked();
	settings.network.online = false;
	config::GGPOEnable = true;
	expectBlocked();
	config::GGPOEnable = false;
	settings.naomi.multiboard = true;
	expectBlocked();
	settings.naomi.multiboard = false;
	settings.naomi.slave = true;
	expectBlocked();
	settings.naomi.slave = false;
	settings.naomi.drivingSimSlave = true;
	expectBlocked();
	settings.naomi.drivingSimSlave = false;
	settings.platform.system = DC_PLATFORM_HIKARU;
	expectBlocked();
	settings.platform.system = DC_PLATFORM_SYSTEMSP;
	expectBlocked();
}

TEST_F(RewindTest, TwoBindingsOnOneSourceNeedTwoReleases)
{
	int deviceSource = 0;
	rewind_state::setRequested(&deviceSource, true);
	rewind_state::setRequested(&deviceSource, true);
	rewind_state::setRequested(&deviceSource, false);
	EXPECT_TRUE(rewind_state::requested());
	rewind_state::setRequested(&deviceSource, false);
	EXPECT_FALSE(rewind_state::requested());
	// Releasing an unknown or already-released source is harmless.
	rewind_state::setRequested(&deviceSource, false);
	EXPECT_FALSE(rewind_state::requested());
	rewind_state::setRequested(&deviceSource, true);
	rewind_state::releaseSource(&deviceSource);
	EXPECT_FALSE(rewind_state::requested());
}

TEST_F(RewindTest, RewindRestorePreservesLivePersistentStorage)
{
	SRamChip sram(256);
	memset(sram.data, 0xAB, sram.size);
	DCFlashChip flash(256);
	memset(flash.data, 0xAB, flash.size);

	std::vector<u8> sramBytes(1024);
	{
		Serializer ser(sramBytes.data(), sramBytes.size());
		sram.Serialize(ser);
		sramBytes.resize(ser.size());
	}
	std::vector<u8> flashBytes(1024);
	{
		Serializer ser(flashBytes.data(), flashBytes.size());
		// DCFlashChip's overrides are private; dispatch through the public base.
		static_cast<MemChip&>(flash).Serialize(ser);
		flashBytes.resize(ser.size());
	}

	// An ordinary loadstate restores the snapshot's persistent data.
	memset(sram.data, 0xCD, sram.size);
	memset(flash.data, 0xCD, flash.size);
	{
		Deserializer deser(sramBytes.data(), sramBytes.size());
		sram.Deserialize(deser);
		EXPECT_EQ(sramBytes.size(), deser.size());
	}
	{
		Deserializer deser(flashBytes.data(), flashBytes.size());
		static_cast<MemChip&>(flash).Deserialize(deser);
		EXPECT_EQ(flashBytes.size(), deser.size());
	}
	EXPECT_EQ(0xAB, sram.data[0]);
	EXPECT_EQ(0xAB, flash.data[0]);

	// A rewind restore consumes the same bytes but keeps the live data, so a
	// later pause/stop can never save an older snapshot over a newer save.
	memset(sram.data, 0xCD, sram.size);
	memset(flash.data, 0xCD, flash.size);
	{
		Deserializer deser(sramBytes.data(), sramBytes.size(), false, true);
		sram.Deserialize(deser);
		EXPECT_EQ(sramBytes.size(), deser.size());
	}
	{
		Deserializer deser(flashBytes.data(), flashBytes.size(), false, true);
		static_cast<MemChip&>(flash).Deserialize(deser);
		EXPECT_EQ(flashBytes.size(), deser.size());
	}
	EXPECT_EQ(0xCD, sram.data[0]);
	EXPECT_EQ(0xCD, sram.data[sram.size - 1]);
	EXPECT_EQ(0xCD, flash.data[0]);
	EXPECT_EQ(0xCD, flash.data[flash.size - 1]);
}

TEST_F(RewindTest, DisabledFeatureReportsUnavailable)
{
	config::RewindEnable = false;
	EXPECT_STREQ("Enable Rewind in Advanced settings before starting a game.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, NoLoadedContentReportsUnavailable)
{
	settings.content.path.clear();
	EXPECT_STREQ("Start a game to record rewind history.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, HardcoreModeReportsUnavailable)
{
	settings.raHardcoreMode = true;
	EXPECT_STREQ("Rewind is unavailable in RetroAchievements hardcore mode.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, GgpoReportsUnavailable)
{
	config::GGPOEnable = true;
	EXPECT_STREQ("Rewind is unavailable during network or linked-cabinet play.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, BattleCableBlocksRewindUntilTheHandshakeEnds)
{
	ASSERT_EQ(nullptr, NetworkHandshake::instance);
	config::BattleCableEnable = true;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	// Creating the handshake reserves the link. Starting it would open the socket.
	NetworkHandshake::init();
	config::BattleCableEnable = false;
	EXPECT_FALSE(rewind_state::requiresSharedThread());
	EXPECT_STREQ("Rewind is unavailable during network or linked-cabinet play.", rewind_state::unavailableReason());
	int source = 0;
	rewind_state::setRequested(&source, true);
	EXPECT_EQ(rewind_state::FrameAction::Run, rewind_state::beforeFrame());
	NetworkHandshake::term();
	EXPECT_TRUE(rewind_state::requiresSharedThread());
}

TEST_F(RewindTest, OnlineAndLinkedCabinetsReportUnavailable)
{
	settings.network.online = true;
	EXPECT_STREQ("Rewind is unavailable during network or linked-cabinet play.", rewind_state::unavailableReason());
	settings.network.online = false;
	settings.naomi.multiboard = true;
	EXPECT_STREQ("Rewind is unavailable during network or linked-cabinet play.", rewind_state::unavailableReason());
	settings.naomi.multiboard = false;
	settings.naomi.slave = true;
	EXPECT_STREQ("Rewind is unavailable during network or linked-cabinet play.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, DisablingFeatureReleasesHeldInput)
{
	int source = 0;
	rewind_state::setRequested(&source, true);
	config::RewindEnable = false;
	rewind_state::poll();
	EXPECT_FALSE(rewind_state::requested());
	EXPECT_EQ(0u, rewind_state::snapshotCount());
}

TEST_F(RewindTest, ThreadedRenderingReportsRestartRequirement)
{
	config::ThreadedRendering = true;
	EXPECT_STREQ("Restart the game to use Rewind with shared emulation and rendering.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, HikaruAndSystemSpReportUnavailable)
{
	settings.platform.system = DC_PLATFORM_HIKARU;
	EXPECT_STREQ("Rewind is not available for this arcade platform.", rewind_state::unavailableReason());
	settings.platform.system = DC_PLATFORM_SYSTEMSP;
	EXPECT_STREQ("Rewind is not available for this arcade platform.", rewind_state::unavailableReason());
}

TEST_F(RewindTest, PollDoesNotCaptureStateForClampedInvalidSettings)
{
	config::RewindInterval = std::numeric_limits<int>::min();
	config::RewindDuration = std::numeric_limits<int>::max();
	config::RewindCompression = std::numeric_limits<int>::min();
	rewind_state::invalidate();
	rewind_state::poll();

	EXPECT_EQ(0u, rewind_state::snapshotCount());
	EXPECT_EQ(0u, rewind_state::memoryUsed());
}

TEST(RewindSerializationTest, RewindRestoreFlagDoesNotChangeSerializedFormat)
{
	struct PlatformRestore
	{
		int system = settings.platform.system;
		u32 ramSize = settings.platform.ram_size;
		~PlatformRestore()
		{
			settings.platform.system = system;
			settings.platform.ram_size = ramSize;
		}
	} restore;
	settings.platform.system = DC_PLATFORM_DREAMCAST;
	settings.platform.ram_size = 16 * 1024 * 1024;

	std::vector<u8> bytes(64);
	Serializer ser(bytes.data(), bytes.size());
	const u32 marker = 0xA17E5A7Eu;
	ser << marker;
	bytes.resize(ser.size());

	Deserializer ordinary(bytes.data(), bytes.size());
	Deserializer rewind(bytes.data(), bytes.size(), false, true);
	u32 ordinaryMarker = 0;
	u32 rewindMarker = 0;
	ordinary >> ordinaryMarker;
	rewind >> rewindMarker;

	EXPECT_FALSE(ordinary.rewind());
	EXPECT_TRUE(rewind.rewind());
	EXPECT_EQ(ordinary.version(), rewind.version());
	EXPECT_EQ(marker, ordinaryMarker);
	EXPECT_EQ(marker, rewindMarker);
	EXPECT_EQ(ordinary.size(), rewind.size());
}
