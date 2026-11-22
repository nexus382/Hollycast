// Portions Copyright 2026 The Hollycast Authors
#include "test_harness_telemetry.h"
#include "test_harness_watch.h"
#include "emulator.h"
#include "gui.h"
#include "performance_overlay.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#ifdef USE_HIKARU
#include "hw/hikaru/frontend.h"
#endif
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace test_harness::telemetry {
using Clock = std::chrono::steady_clock;

static bool initialized;
static std::shared_ptr<const WatchProfile> profile;
static std::uint64_t generation;
static std::uint64_t hikaruFrames, hikaruStamp;
#ifdef USE_HIKARU
static HikaruEngineInputs lastHikaruInputs{};
#endif
static Json previousValues;
static std::uint64_t previousGeneration;

static std::uint64_t stamp()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

struct CpuSample {
	std::uint64_t frames = 0, cycles = 0, microseconds = 0, profileGeneration = 0;
	std::uint32_t pc = 0;
	std::array<std::uint32_t, 64> values{};
	std::array<bool, 64> valid{};
};
static CpuSample cpuSample;
static std::mutex cpuMutex;

static std::uint32_t word(const std::uint8_t *bytes, unsigned size)
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < size; ++i)
		value |= std::uint32_t(bytes[i]) << (i * 8);
	return value;
}

static const char *platformName()
{
	if (settings.platform.isNaomi1()) return "naomi";
	if (settings.platform.isNaomi2()) return "naomi2";
	if (settings.platform.isAtomiswave()) return "atomiswave";
	if (settings.platform.isSystemSP()) return "systemsp";
	return "dreamcast";
}

static void vblank(Event, void *)
{
	// VBlank is dispatched synchronously by the CPU owner. No guest memory is
	// sampled by the UI thread, and no JSON or file I/O runs on this hot path.
	static std::uint64_t frames;
	CpuSample next;
	next.frames = ++frames;
	next.cycles = sh4_sched_now64();
	next.pc = p_sh4rcb ? Sh4cntx.pc : 0;
	next.microseconds = stamp();
	const auto current = std::atomic_load(&profile);
	if (current && current->platform == platformName() && current->gameId == settings.content.gameId)
	{
		next.profileGeneration = current->generation;
		for (std::size_t i = 0; i < current->watches.size(); ++i)
		{
			const auto& watch = current->watches[i];
			if (std::uint64_t(watch.offset) + watch.size <= settings.platform.ram_size)
			{
				next.values[i] = word(&mem_b[watch.offset], watch.size);
				next.valid[i] = true;
			}
		}
	}
	const std::lock_guard<std::mutex> lock(cpuMutex);
	cpuSample = next;
}

void initialize()
{
	if (initialized)
		return;
	EventManager::listen(Event::VBlank, vblank);
	initialized = true;
}

#ifdef USE_HIKARU
void hikaruFrame(const HikaruEngineInputs& inputs)
{
	if (!initialized)
		return;
	// Hikaru runs its complete frame on the UI/render owner. Its memory and
	// dual-CPU registers are exposed only through the serialized frontend API.
	++hikaruFrames;
	hikaruStamp = stamp();
	lastHikaruInputs = inputs;
}
#endif

std::uint64_t completedGuestFrames()
{
#ifdef USE_HIKARU
	if (settings.platform.isHikaru())
		return hikaruFrames;
#endif
	const std::lock_guard<std::mutex> lock(cpuMutex);
	return cpuSample.frames;
}

void configure(const Json& json)
{
	auto next = std::make_shared<WatchProfile>(WatchProfile::fromJson(json));
#ifndef USE_HIKARU
	require(next->platform != "hikaru", "This build does not support Hikaru watch profiles");
#endif
	const char *content = std::getenv("HOLLYCAST_HARNESS_MEDIA_SHA256");
	require(content && next->contentHash == content, "Profile does not match the session's content hash");
	next->generation = ++generation;
	std::atomic_store(&profile, std::shared_ptr<const WatchProfile>(std::move(next)));
	previousValues = Json::object();
	previousGeneration = 0;
}

static unsigned region(const std::string& name)
{
	if (name == "master_ram") return 0;
	if (name == "slave_ram") return 1;
	if (name == "command_ram") return 2;
	if (name == "bram") return 3;
	throw std::runtime_error("Unknown Hikaru RAM region");
}

Json live()
{
	CpuSample cpu;
	{
		const std::lock_guard<std::mutex> lock(cpuMutex);
		cpu = cpuSample;
	}
	const auto current = std::atomic_load(&profile);
	Json machine = Json::object();
	Json values = Json::object();
	Json events = Json::array();
	std::uint64_t sampledAt = cpu.microseconds;
	std::uint64_t frames = cpu.frames;
	bool matched = false;
	std::string mappingStatus = current ? "identity_mismatch" : "unmapped";
	const char *media = std::getenv("HOLLYCAST_HARNESS_MEDIA");
	const bool originalMedia = media && settings.content.path == media;
#ifdef USE_HIKARU
	if (settings.platform.isHikaru())
	{
		frames = hikaruFrames;
		sampledAt = hikaruStamp;
		HikaruHarnessState state{};
		std::array<std::uint32_t, 3> crc{};
		const bool available = hikaru::frontend::harnessState(state, crc);
		machine = {{"platform", "hikaru"}, {"available", available}, {"rom_crc32", crc}};
		if (available)
		{
			const auto executionRegion = [](std::uint32_t pc) {
				const auto physical = pc & 0x1fffffffu;
				return physical < 0x200000 ? "bios_rom"
					: physical >= 0x0c000000 && physical < 0x0e000000 ? "cpu_ram" : "other";
			};
			machine.update({{"master", {{"pc", state.pc[0]}, {"sr", state.sr[0]}, {"state", state.cpu_state[0]}}},
				{"slave", {{"pc", state.pc[1]}, {"sr", state.sr[1]}, {"state", state.cpu_state[1]}}},
				{"cpu_clock_mhz", state.cpu_clock_mhz}, {"ram_bytes", {state.ram_size[0], state.ram_size[1]}}});
			machine["master"]["execution_region"] = executionRegion(state.pc[0]);
			machine["slave"]["execution_region"] = executionRegion(state.pc[1]);
			machine["applied_cabinet_inputs"] = {{"players", {lastHikaruInputs.players[0], lastHikaruInputs.players[1]}},
				{"analog", {lastHikaruInputs.analog[0], lastHikaruInputs.analog[1], lastHikaruInputs.analog[2], lastHikaruInputs.analog[3],
					lastHikaruInputs.analog[4], lastHikaruInputs.analog[5], lastHikaruInputs.analog[6], lastHikaruInputs.analog[7]}},
				{"coins", {lastHikaruInputs.coin[0], lastHikaruInputs.coin[1]}}, {"test", lastHikaruInputs.test}, {"service", lastHikaruInputs.service}};
		}
		matched = available && originalMedia && current && current->platform == "hikaru"
			&& current->gameId == settings.content.gameId && current->romCrc32 == crc;
		if (matched)
			for (const auto& watch : current->watches)
			{
				std::array<std::uint8_t, 4> bytes{};
				const bool valid = hikaru::frontend::harnessRead(region(watch.region), watch.offset, bytes.data(), watch.size);
				values[watch.name] = valid ? watch.decode(word(bytes.data(), watch.size)) : Json{{"value", nullptr}, {"valid", false}, {"reason", "outside_region"}};
			}
	}
	else
#endif
	{
		machine = {{"platform", platformName()}, {"pc", cpu.pc}, {"sh4_cycles", std::to_string(cpu.cycles)}, {"available", sampledAt != 0}};
		matched = originalMedia && current && current->platform == platformName()
			&& current->gameId == settings.content.gameId && cpu.profileGeneration == current->generation;
		if (matched)
			for (std::size_t i = 0; i < current->watches.size(); ++i)
				values[current->watches[i].name] = cpu.valid[i] ? current->watches[i].decode(cpu.values[i])
					: Json{{"value", nullptr}, {"valid", false}, {"reason", "outside_region"}};
	}
	if (matched)
	{
		mappingStatus = "matched_operator_mapping";
		if (previousGeneration == current->generation)
			for (const auto& value : values.items())
				if (previousValues.contains(value.key()) && previousValues[value.key()] != value.value())
					events.push_back({{"kind", "value_changed"}, {"name", value.key()},
						{"before", previousValues[value.key()]}, {"after", value.value()}});
		previousValues = values;
		previousGeneration = current->generation;
	}
	else
	{
		previousValues = Json::object();
		previousGeneration = 0;
	}
	const auto age = sampledAt ? Json(double(stamp() - sampledAt) / 1000.0) : Json();
	const auto metrics = performance_overlay::harnessMetrics();
	const auto optional = [](const auto& value) { return value ? Json(*value) : Json(); };
	Json osd = {{"collecting", metrics.collecting}, {"sampled_at_monotonic_ms", metrics.sampledAtMs},
		{"sample_age_ms", metrics.sampledAtMs ? Json(metrics.sampleAgeMs) : Json()},
		{"game_fps", optional(metrics.gameFps)}, {"output_fps", optional(metrics.outputFps)},
		{"vblank_hz", optional(metrics.vblankHz)}, {"process_cpu_percent", optional(metrics.processCpuPercent)},
		{"emulation_speed_percent", optional(metrics.emulationSpeedPercent)},
		{"system_cpu_percent", optional(metrics.systemCpuPercent)}, {"gpu_busy_percent", optional(metrics.gpuBusyPercent)},
		{"gpu_render_ms", optional(metrics.gpuRenderMs)}, {"frame_average_ms", optional(metrics.frameAverageMs)},
		{"frame_minimum_ms", optional(metrics.frameMinimumMs)}, {"frame_maximum_ms", optional(metrics.frameMaximumMs)},
		{"process_resident_bytes", optional(metrics.processResidentBytes)}};
	return {{"sequence", frames}, {"sample_age_ms", age}, {"sample_available", sampledAt != 0}, {"osd", osd},
		{"stale", !sampledAt || stamp() - sampledAt > 500000}, {"machine", machine},
		{"game_state", {{"status", mappingStatus}, {"profile_id", current ? Json(current->id) : Json()},
			{"values", values}, {"events", events}, {"unknown_fields_are_unmapped", true}}}};
}

Json readMemory(const Json& args)
{
	fields(args, {"region", "offset", "length"});
	const auto offset = std::uint32_t(integer(args.at("offset"), 0, 32 * 1024 * 1024 - 1));
	const auto length = std::size_t(integer(args.at("length"), 1, 4096));
	const auto name = args.value("region", std::string("main_ram"));
	std::vector<std::uint8_t> bytes(length);
	bool paused = false;
#ifdef USE_HIKARU
	if (settings.platform.isHikaru())
		require(hikaru::frontend::harnessRead(region(name), offset, bytes.data(), length), "Hikaru memory range is unavailable");
	else
#endif
	{
		require(name == "main_ram" && emu.running() && !gui_is_open(), "Main RAM snapshot requires running SH-4 gameplay");
		require(std::uint64_t(offset) + length <= settings.platform.ram_size, "Memory range is outside main RAM");
		emu.stop();
		std::memcpy(bytes.data(), &mem_b[offset], length);
		emu.start();
		paused = true;
	}
	std::string hex;
	hex.reserve(length * 2);
	constexpr char digits[] = "0123456789abcdef";
	for (const auto byte : bytes)
	{
		hex.push_back(digits[byte >> 4]);
		hex.push_back(digits[byte & 15]);
	}
	return {{"region", name}, {"offset", offset}, {"length", length}, {"hex", hex},
		{"frame_boundary_snapshot", true}, {"paused_for_snapshot", paused}};
}

} // namespace test_harness::telemetry
