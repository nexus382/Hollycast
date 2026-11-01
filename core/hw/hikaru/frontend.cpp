// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "frontend.h"
#include "roms.h"
#include "mie.h"
#include "protection.h"
#include "sound.h"
#include "audio_pacer.h"
#include "valkyrie/engine.h"
#include "audio/audiostream.h"
#include "archive/archive.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "input/gamepad_device.h"
#include "hw/maple/maple_devs.h"
#include "hw/naomi/naomi_cart.h"
#include "rend/gles/glcache.h"
#include "ui/imgui_driver.h"
#include "ui/gui.h"
#include "ui/performance_overlay.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

namespace hikaru::frontend {
namespace {
std::unique_ptr<RomImages> images;
const GameDescriptor* activeGame = nullptr;
std::unique_ptr<Mie> mie;
std::unique_ptr<Protection> protection;
std::unique_ptr<Sound> sound;
std::unique_ptr<AudioPacer> audioPacer;
bool audioModeKnown = false, previousFastForward = false, previousMuted = false, previousPacing = false;
HikaruEngine* engine = nullptr;
unsigned internalHeight = 0;
uint64_t clocks = 0;
std::string ioError;
std::atomic<bool> resetRequested{false};
std::array<std::atomic<unsigned>, 2> pendingCredits{};
std::array<bool, 2> creditPulse{};
std::array<uint8_t, HIKARU_ENGINE_BRAM_SIZE> savedBram{};
bool hasSavedBram = false;
std::string bramPath;
std::string mieEepromPath;

template<size_t Size>
bool loadNvram(const std::string& path, std::array<uint8_t, Size>& data) {
    errno = 0;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (errno == ENOENT) return false;
        throw std::runtime_error("Cannot open Hikaru nonvolatile data: " + path);
    }
    const bool valid = std::fread(data.data(), 1, data.size(), file) == data.size()
        && std::fgetc(file) == EOF && !std::ferror(file);
    const bool closed = std::fclose(file) == 0;
    if (!valid || !closed) throw std::runtime_error("Invalid or unreadable Hikaru nonvolatile data: " + path);
    return true;
}
template<size_t Size>
bool saveNvram(const std::string& path, const std::array<uint8_t, Size>& data) {
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}
InputDescriptors airTrixInputs = {{
    {NAOMI_START_KEY, "Start"}, {NAOMI_COIN_KEY, "Coin"},
    {NAOMI_BTN2_KEY, "Coin"}, {NAOMI_BTN3_KEY, "Coin"},
    {NAOMI_TEST_KEY, "Test"}, {NAOMI_SERVICE_KEY, "Service"},
    {NAOMI_LEFT_KEY, "Left"}, {NAOMI_RIGHT_KEY, "Right"},
    {NAOMI_UP_KEY, "Kick up"}, {NAOMI_DOWN_KEY, "Kick down"},
}, {
    {"Yaw", Full, 0, false}, {"Roll", Full, 1, false},
}};
InputDescriptors braveFirefightersInputs = {{
    {NAOMI_START_KEY, "Start"}, {NAOMI_COIN_KEY, "Coin"},
    {NAOMI_BTN0_KEY, "Button 1"}, {NAOMI_BTN1_KEY, "Button 2"},
    {NAOMI_TEST_KEY, "Test"}, {NAOMI_SERVICE_KEY, "Service"},
}, {
    {"Horizontal aim", Full, 0, false}, {"Vertical aim", Full, 1, false},
}};

std::unique_ptr<Archive> findArchive(const std::string& game, const char* name) {
    for (const char* ext : {".zip", ".7z"}) {
        std::string filename = std::string(name) + ext;
        try {
            auto sibling = hostfs::storage().getSubPath(hostfs::storage().getParentPath(game), filename);
            if (hostfs::storage().exists(sibling)) {
                std::unique_ptr<Archive> result(OpenArchive(sibling));
                if (result) return result;
            }
        } catch (const hostfs::StorageException&) {}
        auto path = hostfs::findNaomiBios(filename);
        if (!path.empty()) {
            std::unique_ptr<Archive> result(OpenArchive(path));
            if (result) return result;
        }
    }
    throw std::runtime_error(std::string("Missing ") + name + ".zip or .7z in the game or BIOS folders");
}
std::vector<uint8_t> readRom(Archive& archive, const RomFile& rom) {
    std::unique_ptr<ArchiveFile> file(archive.OpenFile(rom.name));
    if (!file) file.reset(archive.OpenFileByCrc(rom.crc32));
    if (!file) throw std::runtime_error(std::string("Missing ROM: ") + rom.name);
    if (file->length() != rom.size) throw std::runtime_error(std::string("Incorrect ROM size: ") + rom.name);
    std::vector<uint8_t> data(rom.size);
    if (file->Read(data.data(), rom.size) != rom.size) throw std::runtime_error(std::string("Cannot read ROM: ") + rom.name);
    validateRom(rom, data);
    return data;
}
HikaruEngineMie callbacks() {
    return {nullptr,
        [](void*) { try { mie->reset(); clocks = 0; } catch (const std::exception& e) { ioError = e.what(); } },
        [](void*, unsigned size, uint32_t addr, uint64_t* value) -> int {
            try { *value = mie->readHost(addr, size); return 0; }
            catch (const std::exception& e) { ioError = e.what(); return -1; }
        },
        [](void*, unsigned size, uint32_t addr, uint64_t value) -> int {
            try { mie->writeHost(addr, uint32_t(value), size); return 0; }
            catch (const std::exception& e) { ioError = e.what(); return -1; }
        },
        [](void*, uint32_t cycles) -> int {
            try { clocks += uint64_t(cycles) * 16; mie->advanceTicks(uint32_t(clocks / 200)); clocks %= 200; return 0; }
            catch (const std::exception& e) { ioError = e.what(); return -1; }
        },
        [](void*, const HikaruEngineInputs* input) {
            try {
                JvsNode::Inputs value;
                value.test = input->test; value.service = input->service;
                for (unsigned i = 0; i < 2; ++i) { value.players[i] = input->players[i]; value.coin[i] = input->coin[i]; }
                for (unsigned i = 0; i < 8; ++i) value.analog[i] = input->analog[i];
                mie->setInputs(value);
            } catch (const std::exception& e) { ioError = e.what(); }
        },
        [](void*, int asserted) -> int {
            try { return mie->setSoundIrq(asserted != 0) ? 1 : 0; }
            catch (const std::exception& e) { ioError = e.what(); return -1; }
        }};
}
HikaruEngineProtection protectionCallbacks() {
    return {nullptr,
        [](void*) { try { protection->reset(); } catch (const std::exception& e) { ioError = e.what(); } },
        [](void*, unsigned size, uint32_t addr, uint64_t* value) -> int {
            try { *value = protection->read(addr, size); return 0; }
            catch (const std::exception& e) { ioError = e.what(); return -1; }
        },
        [](void*, unsigned size, uint32_t addr, uint64_t value) -> int {
            try { protection->write(addr, value, size); return 0; }
            catch (const std::exception& e) { ioError = e.what(); return -1; }
        }};
}
}
void load(const std::string& path) {
    const auto filename = hostfs::storage().getFileInfo(path).name;
    const auto* selectedGame = findGame(get_file_basename(filename));
    if (!selectedGame) throw std::runtime_error("Unsupported Hikaru game archive");
    std::unique_ptr<Archive> game(OpenArchive(path));
    if (!game) throw std::runtime_error("Cannot open Hikaru game archive");
    auto bios = findArchive(path, "hikaru");
    auto mieArchive = findArchive(path, "mie");
    const RomFile* selected = nullptr;
    // The 0.92 BIOS has been exercised with Air Trix. Other known revisions
    // remain valid ROM inputs but have not received equivalent game testing.
    for (unsigned index : {1u, 0u, 2u}) {
        const auto& candidate = BootRoms[index];
        std::unique_ptr<ArchiveFile> file(bios->OpenFile(candidate.name));
        if (!file) file.reset(bios->OpenFileByCrc(candidate.crc32));
        if (file) { selected = &candidate; break; }
    }
    if (!selected) throw std::runtime_error("No supported Hikaru BIOS found in hikaru archive");
    const auto reader = [&](const RomFile& file) {
        return readRom(std::string(file.name) == selected->name ? *bios : *game, file);
    };
    auto loadedImages = std::make_unique<RomImages>(selectedGame == &BraveFirefightersGame
        ? loadBraveFirefighters(reader, selected->name) : loadAirTrix(reader, selected->name));
    auto loadedMie = std::make_unique<Mie>(readRom(*mieArchive, MieBootRom));
    auto loadedProtection = selectedGame->encrypted
        ? std::make_unique<Protection>(RomImages::AirTrixSecurityKey)
        : std::make_unique<Protection>(std::nullopt);
    const std::string savePrefix = selectedGame->identifier;
    auto loadedBramPath = hostfs::getFlashSavePath("", savePrefix + ".hikaru.nvmem");
    std::array<uint8_t, HIKARU_ENGINE_BRAM_SIZE> loadedBram{};
    const bool loadedBramPresent = loadNvram(loadedBramPath, loadedBram);
    auto loadedMiePath = hostfs::getFlashSavePath("", savePrefix + ".mie.eeprom");
    std::array<uint8_t, 128> loadedEeprom{};
    if (loadNvram(loadedMiePath, loadedEeprom))
        loadedMie->restoreEeprom(std::vector<uint8_t>(loadedEeprom.begin(), loadedEeprom.end()));
    images = std::move(loadedImages); mie = std::move(loadedMie); protection = std::move(loadedProtection); clocks = 0; ioError.clear(); resetRequested = false;
    bramPath = std::move(loadedBramPath); mieEepromPath = std::move(loadedMiePath); savedBram = loadedBram; hasSavedBram = loadedBramPresent;
    activeGame = selectedGame;
    for (auto& credits : pendingCredits) credits = 0;
    creditPulse.fill(false);
    NaomiGameInputs = activeGame == &BraveFirefightersGame ? &braveFirefightersInputs : &airTrixInputs;
}
void releaseGraphics() {
    if (audioPacer) { audioPacer->reset(); ClearAudioOutput(); audioPacer.reset(); audioModeKnown = false; }
    if (engine) {
        if (hikaru_engine_get_bram(engine, savedBram.data(), savedBram.size()) == 0) {
            hasSavedBram = true;
            if (!saveNvram(bramPath, savedBram)) os_notify("Could not save Hikaru battery-backed RAM", 10000);
        } else os_notify("Could not read Hikaru battery-backed RAM", 10000);
        hikaru_engine_destroy(engine); engine = nullptr; sound.reset(); internalHeight = 0; glcache.Reset();
        if (!saveNvram(mieEepromPath, mie->eepromContents())) os_notify("Could not save Hikaru I/O EEPROM", 10000);
    }
}
void unload() { releaseGraphics(); protection.reset(); mie.reset(); images.reset(); activeGame = nullptr; ioError.clear(); resetRequested = false; NaomiGameInputs = nullptr; hasSavedBram = false; bramPath.clear(); mieEepromPath.clear(); }
void requestReset() { resetRequested = true; }
void insertCredit(unsigned player) {
    if (player < pendingCredits.size()) pendingCredits[player].fetch_add(1);
}
void pauseAudio() { if (audioPacer) audioPacer->reset(); ClearAudioOutput(); audioModeKnown = false; }
void resumeAudio() { if (audioPacer) audioPacer->reset(); ClearAudioOutput(); audioModeKnown = false; }
bool render() {
    static const bool traceTiming = std::getenv("HIKARU_TRACE_TIMING") != nullptr;
    const auto renderStart = traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!images || !mie || !protection || !activeGame) throw FlycastException("Hikaru ROMs are not loaded");
    // A renderer change requested by the loader is applied by the UI loop
    // after this frame. Do not cast a still-current Vulkan context to GL.
    auto* context = dynamic_cast<GLGraphicsContext*>(GraphicsContext::Instance());
    if (!context) return false;
    if (context->isGLES() || context->getMajorVersion() < 3 ||
        (context->getMajorVersion() == 3 && context->getMinorVersion() < 2) || !glGetString(GL_VERSION))
        throw FlycastException("Hikaru requires a current desktop OpenGL 3.2 context");
    if (settings.display.width <= 0 || settings.display.height <= 0) return false;
    if (resetRequested.exchange(false)) releaseGraphics();
    if (!engine) {
        HikaruEngineRoms roms{images->boot.data(), images->boot.size(), images->program.data(), images->program.size(), images->data.data(), images->data.size()};
        auto io = callbacks(); std::array<char, 512> error{};
        auto security = protectionCallbacks();
        const auto game = activeGame == &BraveFirefightersGame ? HIKARU_ENGINE_GAME_BRAVEFF : HIKARU_ENGINE_GAME_AIRTRIX;
        engine = hikaru_engine_create_game(game, &roms, &io, &security, 0, unsigned(settings.display.width), unsigned(settings.display.height), 1, error.data(), error.size());
        glcache.Reset();
        if (!engine) throw FlycastException(std::string("Hikaru renderer: ") + error.data());
        if (hasSavedBram && hikaru_engine_set_bram(engine, savedBram.data(), savedBram.size()) != 0)
            throw FlycastException("Cannot restore Hikaru battery-backed RAM");
        audioPacer = std::make_unique<AudioPacer>([](void*, s16 left, s16 right) { WriteSample(right, left); }, nullptr);
        audioModeKnown = false;
        sound = std::make_unique<Sound>([](void*, s16 left, s16 right) {
            // Opt-in diagnostics report signal levels only; no audio is saved.
            static const bool trace = std::getenv("HIKARU_TRACE_AUDIO") != nullptr;
            static uint64_t energy = 0;
            static unsigned samples = 0, peak = 0;
            static auto started = std::chrono::steady_clock::now();
            if (trace) {
                const int l = left, r = right;
                energy += uint64_t(l * l) + uint64_t(r * r);
                peak = std::max(peak, unsigned(std::max(std::abs(l), std::abs(r))));
                if (++samples == 44100) {
                    const auto output = audioOutputDiagnostics();
                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
                    started = now;
                    std::fprintf(stderr, "Hikaru audio: frames=%u elapsed_ms=%lld peak=%u mean_square=%llu backend=%s requested=%llu silent=%llu underruns=%llu dropped=%llu mixed=%llu clipped=%llu board_saturated=%llu tempo=%.3f paced=%llu pending=%u max_pending=%u pace_dropped=%llu queued=%u blocked_s=%.3f input_buffered=%u fast_forward=%d muted=%d pacing=%d vsync=%d\n",
                                 samples, static_cast<long long>(elapsed), peak, static_cast<unsigned long long>(energy / (2 * samples)), audioOutputBackend(),
                                 static_cast<unsigned long long>(output.requestedFrames), static_cast<unsigned long long>(output.silentFrames),
                                 static_cast<unsigned long long>(output.underruns), static_cast<unsigned long long>(output.droppedFrames),
                                 static_cast<unsigned long long>(sound->mixedSamples()), static_cast<unsigned long long>(sound->mixedClippedSamples()),
                                 static_cast<unsigned long long>(sound->boardSaturatedSamples()), audioPacer->tempo(),
                                 static_cast<unsigned long long>(audioPacer->outputFrames()), audioPacer->pendingFrames(),
                                 audioPacer->maxPendingFrames(), static_cast<unsigned long long>(audioPacer->discardedFrames()),
                                 AudioOutputQueuedFrames(), audioOutputBlockedSeconds(), audioPacer->bufferedInputFrames(),
                                 int(settings.input.fastForwardMode), int(settings.aica.muteAudio),
                                 int(bool(config::HikaruAudioPacing)), int(bool(config::VSync)));
                    energy = 0; samples = peak = 0;
                }
            }
            submitOutputSample(*audioPacer, left, right, settings.aica.muteAudio,
                               settings.input.fastForwardMode, config::FastForwardAudio);
        }, nullptr);
        auto audio = sound->callbacks();
        if (hikaru_engine_set_sound(engine, &audio) != 0)
            throw FlycastException(std::string("Hikaru sound: ") + hikaru_engine_last_error(engine));
    }
    const bool fastForward = settings.input.fastForwardMode;
    const bool muted = settings.aica.muteAudio;
    const bool pacing = config::HikaruAudioPacing;
    bool clearedAudio = false;
    if (!audioModeKnown || fastForward != previousFastForward || muted != previousMuted || pacing != previousPacing) {
        audioPacer->reset();
        ClearAudioOutput();
        clearedAudio = true;
        audioModeKnown = true;
        previousFastForward = fastForward; previousMuted = muted; previousPacing = pacing;
    }
    if (audioPacer->beginFrame(pacing && !fastForward && !muted, AudioOutputQueuedFrames(), audioOutputBlockedSeconds()) && !clearedAudio)
        ClearAudioOutput();
    const unsigned cpuClockMhz = unsigned(std::clamp(config::Sh4Clock.get(), 100, 300));
    if (int(cpuClockMhz) != config::Sh4Clock.get())
        config::Sh4Clock.set(int(cpuClockMhz));
    if (hikaru_engine_set_cpu_clock_mhz(engine, cpuClockMhz) != 0)
        throw FlycastException(std::string("Hikaru CPU clock: ") + hikaru_engine_last_error(engine));
    const int requestedHeight = config::RenderResolution.get();
    if (unsigned(requestedHeight) != internalHeight) {
        if (hikaru_engine_set_internal_resolution(engine, unsigned(requestedHeight)) != 0)
            throw FlycastException(std::string("Hikaru renderer: ") + hikaru_engine_last_error(engine));
        internalHeight = unsigned(requestedHeight);
    }
    HikaruEngineInputs input{};
    for (auto& axis : input.analog) axis = 0x8000;
    for (unsigned i = 0; i < 2; ++i) {
        auto pressed = [i](u32 mask) { return (kcode[i] & mask) == 0; };
        // The default keyboard maps D to scancode 4 (A on a US layout).
        // SDL's default gamepad
        // has no D binding, but maps its face buttons to C/X in arcade mode.
        input.coin[i] = pressed(DC_BTN_D) || pressed(DC_BTN_C) || pressed(DC_BTN_X);
        // Separate queued menu credits with a release frame so the JVS
        // rising-edge counter sees every request as a distinct coin.
        if (creditPulse[i]) creditPulse[i] = false;
        else if (!input.coin[i] && pendingCredits[i].load() != 0) {
            pendingCredits[i].fetch_sub(1);
            creditPulse[i] = true;
        }
        input.coin[i] = input.coin[i] || creditPulse[i];
        input.players[i] = (pressed(DC_BTN_START) ? 0x8000 : 0) | (pressed(DC_DPAD_LEFT) ? 0x0200 : 0)
            | (pressed(DC_DPAD_RIGHT) ? 0x0100 : 0) | (pressed(DC_DPAD_UP) ? 0x2000 : 0) | (pressed(DC_DPAD_DOWN) ? 0x1000 : 0);
        if (activeGame == &BraveFirefightersGame)
            input.players[i] = (pressed(DC_BTN_START) ? 0x8000 : 0) | (pressed(DC_BTN_A) ? 0x0200 : 0)
                | (pressed(DC_BTN_B) ? 0x0100 : 0) | (pressed(DC_DPAD_UP) ? 0x2000 : 0)
                | (pressed(DC_DPAD_DOWN) ? 0x1000 : 0) | (pressed(DC_DPAD_LEFT) ? 0x0800 : 0)
                | (pressed(DC_DPAD_RIGHT) ? 0x0400 : 0);
    }
    input.test = !(kcode[0] & DC_DPAD2_DOWN); input.service = !(kcode[0] & DC_DPAD2_UP);
    input.analog[0] = uint16_t(int(joyx[0]) + 32768); input.analog[1] = uint16_t(int(joyy[0]) + 32768);
    if (activeGame == &BraveFirefightersGame) {
        input.analog[2] = uint16_t(int(joyx[1]) + 32768);
        input.analog[3] = uint16_t(int(joyy[1]) + 32768);
    }
    GLint target = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    hikaru_engine_resize(engine, uint32_t(target), unsigned(settings.display.width), unsigned(settings.display.height));
    const auto engineStart = traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    int result;
    {
        Sound::FrameScope audioFrame(*sound);
        if (!audioFrame.acquired()) throw FlycastException("Hikaru sound context is already active");
        result = hikaru_engine_run_frame(engine, &input);
    }
    const auto engineEnd = traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    glcache.Reset();
    if (result || !ioError.empty()) throw FlycastException(ioError.empty() ? hikaru_engine_last_error(engine) : ioError);
    performance_overlay::recordGuestFrame();
    performance_overlay::recordSh4Vblank();
    const unsigned internalWidth = ((uint64_t(internalHeight) * 4 + 3) / 6) * 2;
    performance_overlay::recordRenderedResolution(640, 480, internalWidth, internalHeight);
    const int outputWidth = settings.display.width;
    const int outputHeight = settings.display.height;
    const bool widthLimited = int64_t(outputWidth) * 3 <= int64_t(outputHeight) * 4;
    performance_overlay::recordOutputViewport(widthLimited ? outputWidth : int(int64_t(outputHeight) * 4 / 3),
        widthLimited ? int(int64_t(outputWidth) * 3 / 4) : outputHeight);
    if (imguiDriver) {
        imguiDriver->setFrameRendered();
        // Process queued UI actions (including Pause) and render the normal
        // in-game overlays after the Hikaru frame, on the render thread.
        gui_display_osd();
        glcache.Reset();
    }
    if (traceTiming) {
        static unsigned frames;
        static double engineMs, overlayMs, totalMs;
        const auto end = std::chrono::steady_clock::now();
        engineMs += std::chrono::duration<double, std::milli>(engineEnd - engineStart).count();
        overlayMs += std::chrono::duration<double, std::milli>(end - engineEnd).count();
        totalMs += std::chrono::duration<double, std::milli>(end - renderStart).count();
        if (++frames == 60) {
            std::fprintf(stderr, "Hikaru frontend timing: frames=%u engine_ms=%.3f overlay_ms=%.3f total_ms=%.3f cpu_mhz=%u\n",
                         frames, engineMs / frames, overlayMs / frames, totalMs / frames, cpuClockMhz);
            frames = 0; engineMs = overlayMs = totalMs = 0;
        }
    }
    return true;
}
}
