#include "gtest/gtest.h"
#include "build.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_if.h"
#include "log/LogManager.h"
#include "oslib/oslib.h"
#include "stdclass.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

// The headless renderer, declared the same way Renderer_if.cpp declares it.
Renderer *rend_norend();

#if FEAT_SHREC == DYNAREC_IR
#define CPU_BACKEND "shil-vm"
#elif FEAT_SHREC == DYNAREC_JIT
#define CPU_BACKEND "jit"
#else
#define CPU_BACKEND "interp"
#endif

// Headless, unthrottled boot benchmark. In-game frame rate is pinned to 60 by
// the frame limiter, so it cannot show how much host time the SH4 actually
// costs. Running frames back to back with no window and no audio does.
//
// Needs a real BIOS in the Flycast data directory and a disc image:
//   HOLLYCAST_BENCH_DISC=/path/to/game.cdi Hollycast \
//       --gtest_also_run_disabled_tests --gtest_filter='GameBootBench.*'
class GameBootBench : public ::testing::Test {
protected:
	void SetUp() override
	{
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
		emu.start();
	}

	void TearDown() override
	{
		if (emu.running())
			emu.stop();
		emu.unloadGame(false);
		rend_term_renderer();
		os_UninstallFaultHandler();
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

	std::string discPath;
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

	const size_t slash = discPath.find_last_of("/\\");
	const std::string name = slash == std::string::npos ? discPath : discPath.substr(slash + 1);
	std::printf("  %-28s %-9s %7.1f fps   %6.3f ms/frame   (%d frames in %.2fs)\n",
		name.c_str(), CPU_BACKEND, TIMED_FRAMES / seconds, seconds * 1000.0 / TIMED_FRAMES,
		TIMED_FRAMES, seconds);
	fflush(stdout);

	EXPECT_GT(seconds, 0.0);
}
