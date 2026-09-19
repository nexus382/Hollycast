#include "gtest/gtest.h"
#include "build.h"

#include "archive/rzip.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_if.h"
#include "log/LogManager.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "serialize.h"
#include "stdclass.h"

#if FEAT_SHREC == DYNAREC_IR
#include "hw/sh4/dyna/aot_hot_dump.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The headless renderer, declared the same way Renderer_if.cpp declares it.
Renderer *rend_norend();

#if FEAT_SHREC == DYNAREC_IR
#define CPU_BACKEND "shil-vm"
#elif FEAT_SHREC == DYNAREC_JIT
#define CPU_BACKEND "jit"
#else
#define CPU_BACKEND "interp"
#endif

// Headless, unthrottled benchmarks. In-game frame rate is pinned to 60 by the
// frame limiter, so it cannot show how much host time the SH4 actually costs.
// Running frames back to back with no window and no audio does.
//
// Needs a real BIOS in the Flycast data directory and a disc image:
//   HOLLYCAST_BENCH_DISC=/path/to/game.cdi Hollycast \
//       --gtest_also_run_disabled_tests --gtest_filter='GameBootBench.*'
//
// That measures the boot sequence, which is not the code a game spends its time
// in. Adding a savestate saved during play measures gameplay instead:
//   HOLLYCAST_BENCH_STATE=/path/to/game.state HOLLYCAST_BENCH_DISC=... Hollycast ...
class GameBootBench : public ::testing::Test {
protected:
	// Name of the gameplay benchmark below. SetUp() has to know which of the two
	// benchmarks it is preparing, and gtest only exposes that as the test name.
	static constexpr const char *GAMEPLAY_TEST = "DISABLED_GameplayFramePace";

	void SetUp() override
	{
		// One result line per process: bench-boot.sh keeps the best fps of
		// several runs, so a process reporting both a boot and a gameplay figure
		// would have them compared against each other. HOLLYCAST_BENCH_STATE
		// therefore selects the gameplay benchmark and skips the boot one. The
		// choice is made before booting so that the skipped benchmark costs
		// nothing.
		const std::string testName = ::testing::UnitTest::GetInstance()->current_test_info()->name();
		const bool gameplay = testName == GAMEPLAY_TEST;
		const char *stateEnv = getenv("HOLLYCAST_BENCH_STATE");
		if (gameplay)
		{
			if (stateEnv == nullptr)
				GTEST_SKIP() << "set HOLLYCAST_BENCH_STATE to a savestate saved during play";
			statePath = stateEnv;
			if (!file_exists(statePath))
				GTEST_SKIP() << "savestate not found: " << statePath;
		}
		else if (stateEnv != nullptr)
		{
			GTEST_SKIP() << "HOLLYCAST_BENCH_STATE is set: benchmarking gameplay instead of the boot sequence";
		}

		const char *env = getenv("HOLLYCAST_BENCH_DISC");
		if (env == nullptr)
			GTEST_SKIP() << "set HOLLYCAST_BENCH_DISC to a disc image path";
		discPath = env;
		if (!file_exists(discPath))
			GTEST_SKIP() << "disc not found: " << discPath;

		// gtest_main does not run the app's startup, so the data directory
		// holding dc_boot.bin has to be pointed at explicitly.
		const char *home = getenv("HOME");
		if (home == nullptr)
			GTEST_SKIP() << "HOME not set";
		const std::string configDir = std::string(home) + "/Library/Application Support/Flycast/";
		const std::string dataDir = configDir + "data/";
		if (!file_exists(dataDir + "dc_boot.bin"))
			GTEST_SKIP() << "no BIOS at " << dataDir << "dc_boot.bin";
		set_user_config_dir(configDir);
		add_system_data_dir(configDir);
		set_user_data_dir(dataDir);

		LogManager::Init();
		// Emulator::start() write-protects guest RAM for self-modifying code
		// detection. Without the fault handler those writes are a hard crash.
		os_InstallFaultHandler();

		config::UseReios.override(false);
		config::DynarecEnabled.override(true);

		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		emu.loadGame(discPath.c_str());
		// loadGame() applies per-game settings, which undoes overrides, so these
		// have to be re-applied after it. Frames must be driven from this
		// thread, and nothing may pace them — a real audio backend both costs
		// time and throttles the emu thread on buffer backpressure.
		config::ThreadedRendering.override(false);
		config::AudioBackend.override("null");
		// The guest drives rendering itself through PVR register writes, so a
		// renderer has to exist even with no window. rend_init_renderer() keeps
		// whichever renderer is already installed, so seeding it with the
		// headless one avoids ever touching a graphics API.
		renderer = rend_norend();
		if (!rend_init_renderer())
			die("rend_init_renderer failed");
		settings.aica.muteAudio = true;
		// Restored with the machine stopped, which is where gui_loadState() does
		// it too: emu.start() write-protects guest RAM afterwards.
		if (!statePath.empty())
			loadState();
		emu.start();
#if FEAT_SHREC == DYNAREC_IR
		if (gameplay && getenv("HOLLYCAST_AOT_HOT_DUMP") != nullptr)
		{
			aotHotReset();
			aotHotSetProfiling(true);
		}
#endif
	}

	void TearDown() override
	{
#if FEAT_SHREC == DYNAREC_IR
		aotHotSetProfiling(false);
		aotHotReset();
#endif
		if (emu.running())
			emu.stop();
		emu.unloadGame(false);
		rend_term_renderer();
		os_UninstallFaultHandler();
	}

	// Restores the savestate at statePath. dc_loadstate() cannot do it: it
	// resolves the file itself from the game name and a slot number, while the
	// benchmark is pointed at one specific file.
	void loadState()
	{
		std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(statePath, "rb"));
		ASSERT_TRUE(file != nullptr) << "cannot read " << statePath;

		// A savestate starts with the header dc_savestate() writes, see
		// SavestateHeader in nullDC.cpp: magic, creation date, version, and the
		// size of the screenshot that follows it. Older states have no header and
		// start with the serialized data.
		struct Header {
			char magic[8];
			u64 creationDate;
			u32 version;
			u32 pngSize;
		} header;
		static_assert(sizeof(Header) == 24, "savestate header layout changed");
		if (file->read(&header, sizeof(header), 1) == 1
				&& memcmp(header.magic, "FLYSAVE1", sizeof(header.magic)) == 0)
			file->seek(header.pngSize, SEEK_CUR);
		else
			file->seek(0, SEEK_SET);

		std::vector<u8> data;
		size_t bytesRead;
		RZipFile zipFile;
		if (zipFile.Open(file.get(), false))
		{
			file.release();		// RZipFile owns the file from here and closes it
			data.resize(zipFile.Size());
			bytesRead = zipFile.Read(data.data(), data.size());
		}
		else
		{
			// uncompressed savestate
			const s64 start = file->tell();
			file->seek(0, SEEK_END);
			data.resize(file->tell() - start);
			file->seek(start, SEEK_SET);
			bytesRead = file->read(data.data(), 1, data.size());
		}
		ASSERT_EQ(bytesRead, data.size()) << "truncated savestate " << statePath;

		// A state this build cannot read has to fail the benchmark: reporting the
		// un-restored boot under a gameplay label is worse than no figure at all.
		try {
			Deserializer deser(data.data(), data.size());
			emu.loadstate(deser);
		} catch (const Deserializer::Exception& e) {
			FAIL() << "cannot load " << statePath << ": " << e.what();
		}
	}

	// Returns host seconds spent running `frames` emulated frames.
	double runFrames(int frames)
	{
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < frames; i++)
			emu.run();
		const auto t1 = std::chrono::steady_clock::now();
		return std::chrono::duration<double>(t1 - t0).count();
	}

	// Both benchmarks report through here so their lines keep the one format
	// tests/bench-boot.sh parses.
	static void printResult(const std::string& path, int frames, double seconds)
	{
		const size_t slash = path.find_last_of("/\\");
		const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
		std::printf("  %-28s %-9s %7.1f fps   %6.3f ms/frame   (%d frames in %.2fs)\n",
			name.c_str(), CPU_BACKEND, frames / seconds, seconds * 1000.0 / frames,
			frames, seconds);
		fflush(stdout);
	}

	std::string discPath;
	std::string statePath;
};

TEST_F(GameBootBench, DISABLED_FramePace)
{
	constexpr int WARMUP_FRAMES = 600;   // clear the BIOS logos and cold compiles
	constexpr int TIMED_FRAMES = 900;

	// One window only. Consecutive windows cover different parts of the boot
	// sequence, so comparing them measures the content, not the emulator. To
	// average out host noise, run the whole process again: it replays the same
	// frames from reset.
	runFrames(WARMUP_FRAMES);
	const double seconds = runFrames(TIMED_FRAMES);

	printResult(discPath, TIMED_FRAMES, seconds);

	EXPECT_GT(seconds, 0.0);
}

// Same measurement from a savestate, so that the figure comes from the code a
// game actually spends its time in rather than from its boot sequence. Save the
// state where the game is busy - in a race, not on a menu - and note that no
// input is fed, so the guest plays on with the controls at rest.
TEST_F(GameBootBench, DISABLED_GameplayFramePace)
{
	constexpr int WARMUP_FRAMES = 300;   // recompile the code the state lands in
	constexpr int TIMED_FRAMES = 900;

	// SetUp() skips unless it restored the state, so an empty path here means
	// GAMEPLAY_TEST no longer matches this test's name.
	ASSERT_FALSE(statePath.empty());

	runFrames(WARMUP_FRAMES);
	const double seconds = runFrames(TIMED_FRAMES);

	printResult(statePath, TIMED_FRAMES, seconds);

	EXPECT_GT(seconds, 0.0);

#if FEAT_SHREC == DYNAREC_IR
	// Optional profile dump for offline AOT: top hot blocks by visit count.
	if (const char *dumpPath = getenv("HOLLYCAST_AOT_HOT_DUMP"))
	{
		aotHotDumpTopN(dumpPath, 256);
	}
#endif
}
