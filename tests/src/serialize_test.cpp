// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "types.h"
#include "hw/mem/addrspace.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "hw/gdrom/gdrom_if.h"
#include "hw/gdrom/gdromv3.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "emulator.h"
#include "cfg/option.h"
#include "hw/bba/bba.h"
#include "util/tsqueue.h"

#include <vector>

class SerializeTest : public ::testing::Test {
protected:
	static void gdromCommand(const u16 (&packet)[6])
	{
		WriteMem_gdrom(GD_COMMAND_Write, ATA_SPI_PACKET, 1);
		for (u16 word : packet)
			WriteMem_gdrom(GD_DATA, word, 2);
	}

	static void advanceMilliseconds(int milliseconds)
	{
		for (int i = 0; i < milliseconds; i++)
		{
			Sh4cntx.sh4_sched_next -= 1_sh4ms;
			sh4_sched_tick(1_sh4ms);
		}
	}

	static std::vector<char> legacyGdromState(Deserializer::Version version)
	{
		std::vector<char> data(1024);
		Serializer ser(data.data(), data.size());
		gdrom::serialize(ser);
		data.resize(ser.size() - sizeof(int));
		if (version == Deserializer::V61)
		{
			// V61 stored data_write_mode before the registers and no GD-ROM timers.
			data.resize(data.size() - 6 * sizeof(int));
			const size_t registersSize = sizeof(u32) + sizeof(GD_ErrRegT)
					+ sizeof(GD_InterruptReasonT) + sizeof(GD_FeaturesT)
					+ sizeof(GD_SecCountT) + sizeof(GD_SecNumbT)
					+ sizeof(GD_StatusT) + sizeof(ByteCount_t);
			data.insert(data.end() - registersSize, sizeof(u32), 0);
		}
		std::memcpy(data.data(), &version, sizeof(version));
		return data;
	}

	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		emu.dc_reset(true);
	}
};

TEST_F(SerializeTest, SizeTest)
{
	using namespace config;
	Settings::instance().reset();
	MapleMainDevices[0] = MDT_SegaController;
	MapleExpansionDevices[0][0] = MDT_SegaVMU;
	MapleExpansionDevices[0][1] = MDT_SegaVMU;
	MapleMainDevices[1] = MDT_SegaController;
	MapleExpansionDevices[1][0] = MDT_SegaVMU;
	MapleExpansionDevices[1][1] = MDT_SegaVMU;
	mcfg_CreateDevices();

	std::vector<char> data(30000000);
	Serializer ser(data.data(), data.size());
	dc_serialize(ser);
	ASSERT_EQ(28050910u, ser.size());
}

TEST_F(SerializeTest, GdromPendingSeekSurvivesSaveLoad)
{
	const u16 seek[6] = { SPI_CD_SEEK | (1 << 8), 0, 150, 0, 0, 0 };
	const u16 stop[6] = { SPI_CD_SEEK | (3 << 8), 0, 0, 0, 0, 0 };
	gdromCommand(seek);
	advanceMilliseconds(7);
	ASSERT_EQ(GD_SEEK, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);
	ASSERT_EQ(0u, ReadMem_gdrom(GD_ALTSTAT_Read, 1) & 0x10);

	std::vector<char> data(30000000);
	Serializer ser(data.data(), data.size());
	dc_serialize(ser);

	gdromCommand(stop);
	advanceMilliseconds(6);
	ASSERT_EQ(GD_STANDBY, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);

	Deserializer deser(data.data(), ser.size());
	dc_deserialize(deser);
	advanceMilliseconds(12);
	ASSERT_EQ(GD_SEEK, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);
	ASSERT_EQ(0u, ReadMem_gdrom(GD_ALTSTAT_Read, 1) & 0x10);
	advanceMilliseconds(2);
	EXPECT_EQ(GD_PAUSE, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);
	EXPECT_NE(0u, ReadMem_gdrom(GD_ALTSTAT_Read, 1) & 0x10);
}

TEST_F(SerializeTest, GdromV62PendingTransitionFallsBackToPause)
{
	const u16 stop[6] = { SPI_CD_SEEK | (3 << 8), 0, 0, 0, 0, 0 };
	gdromCommand(stop);
	auto data = legacyGdromState(Deserializer::V62);
	ASSERT_EQ(GD_BUSY, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);

	Deserializer deser(data.data(), data.size());
	gdrom::deserialize(deser);
	sh4_sched_ffts();
	advanceMilliseconds(6);
	EXPECT_EQ(GD_PAUSE, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);
	EXPECT_NE(0u, ReadMem_gdrom(GD_ALTSTAT_Read, 1) & 0x10);
}

TEST_F(SerializeTest, GdromPendingStopSurvivesSaveLoad)
{
	const u16 stop[6] = { SPI_CD_SEEK | (3 << 8), 0, 0, 0, 0, 0 };
	const u16 seek[6] = { SPI_CD_SEEK | (1 << 8), 0, 150, 0, 0, 0 };
	gdromCommand(stop);
	std::vector<char> data(30000000);
	Serializer ser(data.data(), data.size());
	dc_serialize(ser);
	gdromCommand(seek);
	advanceMilliseconds(21);
	ASSERT_EQ(GD_PAUSE, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);

	Deserializer deser(data.data(), ser.size());
	dc_deserialize(deser);
	advanceMilliseconds(6);
	EXPECT_EQ(GD_STANDBY, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);
}

TEST_F(SerializeTest, GdromV61LoadCancelsPendingTransition)
{
	const u16 pause[6] = { SPI_CD_SEEK | (4 << 8), 0, 0, 0, 0, 0 };
	const u16 stop[6] = { SPI_CD_SEEK | (3 << 8), 0, 0, 0, 0, 0 };
	gdromCommand(pause);
	auto data = legacyGdromState(Deserializer::V61);
	gdromCommand(stop);
	ASSERT_EQ(GD_BUSY, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);

	Deserializer deser(data.data(), data.size());
	gdrom::deserialize(deser);
	sh4_sched_ffts();
	advanceMilliseconds(30);
	EXPECT_EQ(GD_PAUSE, ReadMem_gdrom(GD_SECTNUM, 1) & 0xf);
}

TEST(SerializerBufferTest, BufferOverflowThrowsException)
{
	std::vector<char> smallBuffer(10);
	Serializer ser(smallBuffer.data(), smallBuffer.size());
	std::vector<int> largeData(100);
	ASSERT_THROW(ser.serialize(largeData.data(), largeData.size()), Serializer::Exception);
}

TEST(SerializerBufferTest, BufferWithinLimitSucceeds)
{
	std::vector<char> buffer(100);
	Serializer ser(buffer.data(), buffer.size());

	// Record the header size (which includes e.g. version)
	size_t initialSize = ser.size();

	std::vector<int> data(10);
	ASSERT_NO_THROW(ser.serialize(data.data(), data.size()));
	ASSERT_EQ(ser.size(), initialSize + sizeof(int) * 10);
}

TEST(SerializerBufferTest, ExactBufferSizeSucceeds)
{
	// Determine the header size (which includes e.g. version)
	Serializer dryRun(nullptr, 1000);
	size_t headerSize = dryRun.size();

	// Serialize data to buffer of equal size
	std::vector<char> buffer(headerSize + sizeof(int) * 10);
	std::vector<int> data(10);
	Serializer ser(buffer.data(), buffer.size());
	ASSERT_NO_THROW(ser.serialize(data.data(), data.size()));
	ASSERT_EQ(ser.size(), buffer.size());
}

TEST_F(SerializeTest, BbaStateSkipsDevicePayloadWhenRuntimeDeviceIsMissing)
{
	if (!settings.platform.isConsole() && !settings.platform.isAtomiswave())
		GTEST_SKIP() << "BBA device is unavailable on this platform";

	struct BbaConfigRestore
	{
		bool enabled = config::EmulateBBA;
		~BbaConfigRestore()
		{
			config::EmulateBBA = enabled;
			bba_Reset(true);
		}
	} restore;

	config::EmulateBBA = true;
	bba_Reset(true);
	std::vector<char> data(100000);
	Serializer ser(data.data(), data.size());
	bba_Serialize(ser);
	data.resize(ser.size());

	// Simulate loading a BBA-enabled V63 state where this runtime has no BBA.
	bba_Term();
	std::vector<char> noDeviceData(100000);
	Serializer noDeviceSer(noDeviceData.data(), noDeviceData.size());
	bba_Serialize(noDeviceSer);
	EXPECT_EQ(ser.size(), noDeviceSer.size());
	noDeviceData.resize(noDeviceSer.size());
	Deserializer deser(data.data(), data.size());
	EXPECT_NO_THROW(bba_Deserialize(deser));
	EXPECT_EQ(ser.size(), deser.size());
}

TEST(NetworkFrameQueueTest, TryPopDrainsFramesInFifoOrder)
{
	TsQueue<std::vector<u8>> queue;
	std::vector<u8> frame;
	EXPECT_FALSE(queue.tryPop(frame));
	queue.push(std::vector<u8>{ 1, 2, 3 });
	queue.push(std::vector<u8>{ 4, 5 });
	ASSERT_TRUE(queue.tryPop(frame));
	EXPECT_EQ((std::vector<u8>{ 1, 2, 3 }), frame);
	ASSERT_TRUE(queue.tryPop(frame));
	EXPECT_EQ((std::vector<u8>{ 4, 5 }), frame);
	EXPECT_FALSE(queue.tryPop(frame));
}

TEST(SerializerBufferTest, OffByOneOverflowThrowsException)
{
	// Create a buffer that's 1 byte too small
	std::vector<char> buffer(sizeof(int) * 10 - 1);
	std::vector<int> data(10);
	Serializer ser(buffer.data(), buffer.size());
	ASSERT_THROW(ser.serialize(data.data(), data.size()), Serializer::Exception);
}
