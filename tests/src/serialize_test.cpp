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
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "serialize.h"
#include "ui/gui.h"
#include "stdclass.h"
#include "util/tsqueue.h"

#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

class RestoreEvents
{
	static constexpr Event observed[] = {Event::LoadState, Event::Pause, Event::Resume, Event::Terminate};
	static void record(Event event, void *context)
	{
		static_cast<RestoreEvents *>(context)->events.push_back(event);
	}

public:
	std::vector<Event> events;
	RestoreEvents()
	{
		for (Event event : observed)
			EventManager::listen(event, record, this);
	}
	~RestoreEvents()
	{
		for (Event event : observed)
			EventManager::unlisten(event, record, this);
	}
};

std::filesystem::path writeStateFixture(const std::vector<char>& data, const char *name)
{
	static std::atomic<unsigned> sequence{0};
	const auto path = std::filesystem::temp_directory_path()
			/ (std::string("hollycast-") + name + "-" + std::to_string(sequence++) + ".state");
	std::ofstream file(path, std::ios::binary);
	file.write(data.data(), data.size());
	return path;
}

std::vector<char> serializeDreamcastState()
{
	std::vector<char> data(30'000'000);
	Serializer serializer(data.data(), data.size());
	dc_serialize(serializer);
	data.resize(serializer.size());
	return data;
}

} // namespace

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
		// Non-Hikaru headers carry no platform tag, exactly as in dev.
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

TEST_F(SerializeTest, DreamcastHeaderMatchesDevV63)
{
	std::vector<char> data(64);
	Serializer ser(data.data(), data.size());
	u32 version = 0, ramSize = 0;
	std::memcpy(&version, data.data(), sizeof(version));
	std::memcpy(&ramSize, data.data() + sizeof(u32), sizeof(ramSize));
	EXPECT_EQ(u32(Deserializer::V63), version);
	EXPECT_EQ(settings.platform.ram_size, ramSize);
	EXPECT_EQ(2 * sizeof(u32), ser.size());
}

TEST_F(SerializeTest, PartialRestoreSuppressesRestartAndAutosave)
{
	struct RestoreSettings
	{
		std::string contentPath = settings.content.path;
		std::string fileName = settings.content.fileName;
		std::string gameId = settings.content.gameId;
		std::string dataPath = get_writable_data_path("");
		int broadcast = config::Broadcast;
		bool autoSave = config::AutoSaveState;
		bool threaded = config::ThreadedRendering;
		std::string audioBackend = config::AudioBackend;
		GuiState gui = gui_state;
		~RestoreSettings()
		{
			settings.content.path = contentPath;
			settings.content.fileName = fileName;
			settings.content.gameId = gameId;
			config::Broadcast = broadcast;
			config::AutoSaveState = autoSave;
			config::ThreadedRendering = threaded;
			config::AudioBackend = audioBackend;
			gui_state = gui;
			set_user_data_dir(dataPath);
		}
	} restoreSettings;
	struct RestoreMapleConfiguration
	{
		std::array<MapleDeviceType, 4> main{};
		std::array<std::array<MapleDeviceType, 2>, 4> expansion{};
		RestoreMapleConfiguration()
		{
			for (size_t bus = 0; bus < main.size(); bus++)
			{
				main[bus] = config::MapleMainDevices[bus];
				config::MapleMainDevices[bus] = MDT_None;
				for (size_t port = 0; port < expansion[bus].size(); port++)
				{
					expansion[bus][port] = config::MapleExpansionDevices[bus][port];
					config::MapleExpansionDevices[bus][port] = MDT_None;
				}
			}
		}
		~RestoreMapleConfiguration()
		{
			for (size_t bus = 0; bus < main.size(); bus++)
			{
				config::MapleMainDevices[bus] = main[bus];
				for (size_t port = 0; port < expansion[bus].size(); port++)
					config::MapleExpansionDevices[bus][port] = expansion[bus][port];
			}
		}
	} restoreMapleConfiguration;
	EXPECT_FALSE(dc_consumeAutoSaveSkipOnNextUnload());

	const auto directory = std::filesystem::temp_directory_path() / ("hollycast-partial-restore-"
			+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	set_user_data_dir(directory.string());
	const auto gamePath = directory / "partial-restore-test.elf";
	std::ofstream(gamePath, std::ios::binary).put('\0');
	// Keep this synthetic session independent of configured VMU devices.
	mcfg_DestroyDevices(false);
	emu.loadGame(gamePath.string().c_str());
	config::AutoSaveState = true;
	const int autoSaveSlot = dc_getAutoSaveSlot();
	const std::string autoSavePath = hostfs::getSavestatePath(autoSaveSlot, true);
	const std::string savedMarker = "preserve known-good autosave";
	{ std::ofstream marker(autoSavePath, std::ios::binary); marker << savedMarker; }
	config::Broadcast = 99;
	const auto partialPath = writeStateFixture(serializeDreamcastState(), "partial-restore");
	config::Broadcast = 0;
	const auto validPath = writeStateFixture(serializeDreamcastState(), "valid-restore");
	const auto earlyRejectPath = writeStateFixture(std::vector<char>(sizeof(u32), 0), "early-reject");

	RestoreEvents observed;
	EXPECT_EQ(StateLoadResult::Rejected, dc_loadstateForPathResult(earlyRejectPath.string()));
	const int numberedSlot = (autoSaveSlot + 1) % NUM_SAVE_SLOTS;
	const auto numberedPath = hostfs::getSavestatePath(numberedSlot, true);
	std::filesystem::copy_file(earlyRejectPath, numberedPath);
	// In single-threaded mode the guest only advances if the frontend asks for a frame.
	config::ThreadedRendering = false;
	config::AudioBackend = "null";
	gui_state = GuiState::Closed;
	emu.start();
	EXPECT_TRUE(emu.running());
	observed.events.clear();
	gui_loadState(numberedSlot);
	EXPECT_TRUE(emu.running());
	EXPECT_EQ((std::vector<Event>{Event::Pause, Event::Resume}), observed.events);
	emu.stop();
	observed.events.clear();
	EXPECT_EQ(StateLoadResult::PartialApplyFailure, dc_loadstateForPathResult(partialPath.string()));
	EXPECT_FALSE(emu.running());
	EXPECT_TRUE(observed.events.empty());
	emu.start();
	EXPECT_FALSE(emu.running());
	std::filesystem::copy_file(partialPath, numberedPath, std::filesystem::copy_options::overwrite_existing);
	gui_loadState(numberedSlot);
	EXPECT_EQ(GuiState::Main, gui_state);
	EXPECT_EQ((std::vector<Event>{Event::Terminate}), observed.events);
	{
		std::ifstream marker(autoSavePath, std::ios::binary);
		const std::string contents((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
		EXPECT_EQ(savedMarker, contents);
	}
	EXPECT_FALSE(dc_consumeAutoSaveSkipOnNextUnload());
	emu.loadGame(gamePath.string().c_str());
	config::AutoSaveState = true;
	observed.events.clear();
	EXPECT_EQ(StateLoadResult::Loaded, dc_loadstateForPathResult(validPath.string()));
	EXPECT_FALSE(emu.running());
	EXPECT_TRUE(dc_loadstateForPath(validPath.string()));
	EXPECT_EQ((std::vector<Event>{Event::LoadState, Event::LoadState}), observed.events);
	emu.unloadGame();
	EXPECT_GT(std::filesystem::file_size(autoSavePath), savedMarker.size());
	emu.loadGame(gamePath.string().c_str());
	observed.events.clear();
	EXPECT_EQ(StateLoadResult::Loaded, dc_loadstateResult(autoSaveSlot));
	EXPECT_EQ((std::vector<Event>{Event::LoadState}), observed.events);
	emu.unloadGame(false);

	std::filesystem::remove(partialPath);
	std::filesystem::remove(validPath);
	std::filesystem::remove(earlyRejectPath);
	std::filesystem::remove_all(directory);
}

TEST_F(SerializeTest, TaggedV64DreamcastHeaderStillLoads)
{
	// States written by earlier Hikaru test builds carry the V64 platform
	// tag even for Dreamcast; the reader must keep consuming it.
	std::vector<char> data(64);
	Serializer ser(data.data(), data.size());
	const u32 marker = 0x12345678;
	ser << marker;
	data.resize(ser.size());

	std::vector<char> tagged = data;
	const u32 platform = u32(DC_PLATFORM_DREAMCAST);
	tagged.insert(tagged.begin() + sizeof(u32), sizeof(u32), 0);
	std::memcpy(tagged.data() + sizeof(u32), &platform, sizeof(platform));
	const u32 version = u32(Deserializer::V64);
	std::memcpy(tagged.data(), &version, sizeof(version));

	Deserializer deser(tagged.data(), tagged.size());
	EXPECT_EQ(Deserializer::V64, deser.version());
	u32 readMarker = 0;
	deser >> readMarker;
	EXPECT_EQ(marker, readMarker);
}

#ifdef USE_HIKARU
TEST_F(SerializeTest, HikaruHeaderIsTaggedV64)
{
	struct PlatformRestore {
		int system = settings.platform.system;
		~PlatformRestore() { settings.platform.system = system; }
	} restore;
	settings.platform.system = DC_PLATFORM_HIKARU;

	std::vector<char> data(64);
	Serializer ser(data.data(), data.size());
	u32 version = 0, platform = 0;
	std::memcpy(&version, data.data(), sizeof(version));
	std::memcpy(&platform, data.data() + sizeof(u32), sizeof(platform));
	EXPECT_EQ(u32(Deserializer::V64), version);
	EXPECT_EQ(u32(DC_PLATFORM_HIKARU), platform);
	EXPECT_EQ(2 * sizeof(u32), ser.size());
	data.resize(ser.size());

	Deserializer deser(data.data(), data.size());
	EXPECT_EQ(Deserializer::V64, deser.version());
	settings.platform.system = DC_PLATFORM_DREAMCAST;
	EXPECT_THROW(Deserializer(data.data(), data.size()), Deserializer::Exception);
}
#endif

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
	Serializer measure;
	std::vector<char> smallBuffer(measure.size() + 10);
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
