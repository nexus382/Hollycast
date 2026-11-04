// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "frontend.h"
#include "input_bindings.h"
#include "cabinet_settings.h"
#include "roms.h"
#include "nvram.h"
#include "mie.h"
#include "protection.h"
#include "sound.h"
#include "audio_pacer.h"
#include "graphics/device.h"
#include "valkyrie/engine.h"
#include "audio/audiostream.h"
#include "archive/archive.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "oslib/directory.h"
#include "input/gamepad_device.h"
#include "input/mouse.h"
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
#include <optional>
#include <stdexcept>
#include <vector>

namespace hikaru::frontend {
namespace {
std::unique_ptr<RomImages> images;
std::atomic<const GameDescriptor*> activeGame{nullptr};
std::unique_ptr<Mie> mie;
std::unique_ptr<Protection> protection;
std::unique_ptr<Sound> sound;
std::unique_ptr<AudioPacer> audioPacer;
bool audioModeKnown = false, previousFastForward = false, previousMuted = false, previousPacing = false;
HikaruEngine* engine = nullptr;
std::unique_ptr<graphics::Device> nativeGraphics;
unsigned internalHeight = 0;
uint64_t clocks = 0;
std::string ioError;
std::atomic<bool> resetRequested{false};
std::atomic<bool> regionRequested{false};
std::array<CoinPulseQueue, 2> credits;
std::atomic<bool> freshAudioOutput{false};
std::array<uint8_t, HIKARU_ENGINE_BRAM_SIZE> savedBram{};
bool hasSavedBram = false;
bool bramSaveAllowed = true, mieSaveAllowed = true;
std::string bramPath;
std::string mieEepromPath;
std::array<uint8_t, 4> cabinetId{};
CabinetRegion pendingRegion = CabinetRegion::Saved;

CabinetRegion configuredRegion(const GameDescriptor* game) {
    const int value = game == &AirTrixGame ? config::HikaruAirTrixRegion.get()
        : game == &BraveFirefightersGame ? config::HikaruBraveRegion.get() : config::HikaruPlanetRegion.get();
    return value >= -1 && value <= 2 ? static_cast<CabinetRegion>(value) : CabinetRegion::Saved;
}

template<size_t Size>
bool saveNvram(const std::string& path, const std::array<uint8_t, Size>& data) {
    const std::string temporary = path + ".tmp";
    FILE* file = nowide::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    const bool closed = std::fclose(file) == 0;
    // flycast::rename replaces an existing save and accepts UTF-8 on Windows.
    if (!written || !closed || flycast::rename(temporary.c_str(), path.c_str()) != 0) {
        nowide::remove(temporary.c_str());
        return false;
    }
    return true;
}
bool backupCabinetSettings(const std::array<uint8_t, 128>& eeprom) {
    if (!bramSaveAllowed || !mieSaveAllowed) return false;
    auto bram = savedBram;
    if (engine && hikaru_engine_get_bram(engine, bram.data(), bram.size()) != 0) return false;
    // Capture live records too: the game may hold newer scores than the disk.
    // The first backup of either file is never overwritten by later changes.
    return backupNvram(mieEepromPath + ".before-region", eeprom)
        && (!(engine || hasSavedBram) || backupNvram(bramPath + ".before-region", bram));
}
InputDescriptors airTrixInputs = {{
    {NAOMI_START_KEY, "Start"}, {NAOMI_COIN_KEY, "Coin"},
    {NAOMI_BTN0_KEY, "Tail kick"}, {NAOMI_BTN1_KEY, "Nose kick"},
    {NAOMI_BTN2_KEY, "Cabinet left"}, {NAOMI_BTN3_KEY, "Cabinet right"},
    {NAOMI_BTN4_KEY, "Coin"},
    {NAOMI_TEST_KEY, "Test"}, {NAOMI_SERVICE_KEY, "Service"},
    {NAOMI_LEFT_KEY, "Left"}, {NAOMI_RIGHT_KEY, "Right"},
    {NAOMI_UP_KEY, "Up"}, {NAOMI_DOWN_KEY, "Down"},
}, {
    {"Yaw", Full, 0, false}, {"Roll", Full, 1, false},
}};
InputDescriptors braveFirefightersInputs = {{
    {NAOMI_START_KEY, "Start"}, {NAOMI_COIN_KEY, "Coin"},
    {NAOMI_BTN0_KEY, "Spray"}, {NAOMI_BTN1_KEY, "Fog"},
    {NAOMI_TEST_KEY, "Test"}, {NAOMI_SERVICE_KEY, "Service"},
}, {
    {"Horizontal aim", Full, 0, false}, {"Vertical aim", Full, 1, false},
}};

InputDescriptors planetHarriersInputs = {{
    {NAOMI_START_KEY, "Start"}, {NAOMI_COIN_KEY, "Coin"},
    {NAOMI_BTN0_KEY, "Bullet"}, {NAOMI_BTN1_KEY, "Missile"},
    {NAOMI_BTN2_KEY, "Bomb"}, {NAOMI_BTN3_KEY, "View left"},
    {NAOMI_BTN4_KEY, "View right"},
    {NAOMI_TEST_KEY, "Test"}, {NAOMI_SERVICE_KEY, "Service"},
}, {
    {"Stick horizontal", Full, 0, false}, {"Stick vertical", Full, 1, false},
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
    RomImages loadedRoms;
    if (selectedGame == &PlanetHarriersGame)
        loadedRoms = loadPlanetHarriers(reader, selected->name);
    else if (selectedGame == &BraveFirefightersGame)
        loadedRoms = loadBraveFirefighters(reader, selected->name);
    else
        loadedRoms = loadAirTrix(reader, selected->name);
    auto loadedImages = std::make_unique<RomImages>(std::move(loadedRoms));
    auto loadedMie = std::make_unique<Mie>(readRom(*mieArchive, MieBootRom));
    auto loadedProtection = std::make_unique<Protection>(selectedGame->securityKey);
    const std::string savePrefix = selectedGame->identifier;
    auto loadedBramPath = hostfs::getFlashSavePath("", savePrefix + ".hikaru.nvmem");
    std::array<uint8_t, HIKARU_ENGINE_BRAM_SIZE> loadedBram{};
    const auto bramResult = loadNvram(loadedBramPath, loadedBram);
    auto loadedMiePath = hostfs::getFlashSavePath("", savePrefix + ".mie.eeprom");
    std::array<uint8_t, 128> loadedEeprom{};
    const auto mieResult = loadNvram(loadedMiePath, loadedEeprom);
    if (mieResult == NvramLoadResult::Loaded)
        loadedMie->restoreEeprom(std::vector<uint8_t>(loadedEeprom.begin(), loadedEeprom.end()));
    images = std::move(loadedImages); mie = std::move(loadedMie); protection = std::move(loadedProtection); clocks = 0; ioError.clear(); resetRequested = false;
    bramPath = std::move(loadedBramPath); mieEepromPath = std::move(loadedMiePath); savedBram = loadedBram; hasSavedBram = bramResult == NvramLoadResult::Loaded;
    // Preserve unreadable files for recovery; do not overwrite them on exit.
    bramSaveAllowed = bramResult != NvramLoadResult::Invalid;
    mieSaveAllowed = mieResult != NvramLoadResult::Invalid;
    if (!bramSaveAllowed || !mieSaveAllowed)
        os_notify("Hikaru save data could not be loaded. Using defaults; the original files will be preserved.", 10000);
    for (auto& queue : credits) queue.reset();
    NaomiGameInputs = selectedGame == &PlanetHarriersGame ? &planetHarriersInputs
        : selectedGame == &BraveFirefightersGame ? &braveFirefightersInputs : &airTrixInputs;
    // The cartridge header supplies the BIOS's four-byte cabinet identifier.
    std::copy_n(images->program.begin() + 0x136, cabinetId.size(), cabinetId.begin());
    pendingRegion = configuredRegion(selectedGame);
    regionRequested = false;
    activeGame = selectedGame;
}
void releaseGraphics() {
    graphics::Device::Scope graphicsScope(nativeGraphics.get());
    if (audioPacer) { audioPacer->reset(); audioPacer.reset(); audioModeKnown = false; }
    if (engine) {
        if (hikaru_engine_get_bram(engine, savedBram.data(), savedBram.size()) == 0) {
            hasSavedBram = true;
            if (bramSaveAllowed && !saveNvram(bramPath, savedBram)) os_notify("Could not save Hikaru battery-backed RAM", 10000);
        } else os_notify("Could not read Hikaru battery-backed RAM", 10000);
        hikaru_engine_destroy(engine); engine = nullptr; sound.reset(); internalHeight = 0; glcache.Reset();
        if (mieSaveAllowed && !saveNvram(mieEepromPath, mie->eepromContents())) os_notify("Could not save Hikaru I/O EEPROM", 10000);
    }
    nativeGraphics.reset();
}
void suspendGraphics() {
    graphics::Device::Scope graphicsScope(nativeGraphics.get());
    if (!engine) { nativeGraphics.reset(); return; }
    if (hikaru_engine_get_bram(engine, savedBram.data(), savedBram.size()) == 0) {
        hasSavedBram = true;
        if (bramSaveAllowed && !saveNvram(bramPath, savedBram)) os_notify("Could not save Hikaru battery-backed RAM", 10000);
    }
    if (mieSaveAllowed && !saveNvram(mieEepromPath, mie->eepromContents())) os_notify("Could not save Hikaru I/O EEPROM", 10000);
    pauseAudio();
    ClearAudioOutput();
    freshAudioOutput = true;
    if (hikaru_engine_release_graphics(engine) != 0)
        os_notify("Could not release Hikaru graphics", 10000);
    internalHeight = 0;
    glcache.Reset();
    nativeGraphics.reset();
}
const GameDescriptor* loadedGame() { return activeGame.load(); }
void unload() { activeGame = nullptr; pendingRegion = CabinetRegion::Saved; releaseGraphics(); protection.reset(); mie.reset(); images.reset(); ioError.clear(); resetRequested = false; NaomiGameInputs = nullptr; hasSavedBram = false; bramPath.clear(); mieEepromPath.clear(); }
void requestReset() { resetRequested = true; }
void requestCabinetRegion() { regionRequested = true; }
void insertCredit(unsigned player) {
    if (player < credits.size()) credits[player].enqueue();
}
void pauseAudio() { if (audioPacer) audioPacer->reset(); audioModeKnown = false; }
// InitAudio has already supplied an empty backend. Defer DSP work to its
// producer thread and avoid reopening non-queue backends to clear it again.
void resumeAudio() { freshAudioOutput = true; }
static bool renderFrame() {
    static const bool traceTiming = std::getenv("HIKARU_TRACE_TIMING") != nullptr;
    const auto renderStart = traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!images || !mie || !protection || !activeGame) throw FlycastException("Hikaru ROMs are not loaded");
    // A requested API takes effect after the current UI frame has completed.
    // Never use a new backend with the context that is being replaced.
    auto* graphicsContext = GraphicsContext::Instance();
    if (!graphicsContext) return false;
    const auto requested = config::RendererType.get();
    const auto current = graphicsContext->renderType();
    const bool gl = isOpenGL(requested);
    if (isOpenGL(current) != gl || isVulkan(current) != isVulkan(requested)
            || isMetal(current) != isMetal(requested)) return false;
    if (!gl && !isMetal(requested) && !isVulkan(requested))
        throw FlycastException("The selected graphics API has no Hikaru renderer");
    if (gl) {
        auto* context = dynamic_cast<GLGraphicsContext*>(graphicsContext);
        if (!context || context->getMajorVersion() < 3 ||
            (!context->isGLES() && context->getMajorVersion() == 3 && context->getMinorVersion() < 2) || !glGetString(GL_VERSION))
            throw FlycastException("Hikaru OpenGL requires OpenGL 3.2 or OpenGL ES 3.0");
    }
    if (settings.display.width <= 0 || settings.display.height <= 0) return false;
    if (resetRequested.exchange(false)) {
        releaseGraphics(); ClearAudioOutput(); freshAudioOutput = true;
        pendingRegion = configuredRegion(loadedGame());
    }
    if (regionRequested.exchange(false)) pendingRegion = configuredRegion(loadedGame());
    if (pendingRegion != CabinetRegion::Saved && mie->eepromIdle()) {
        const auto original = mie->eepromContents();
        auto updated = original;
        const auto regionResult = applyCabinetRegion(updated, cabinetId, pendingRegion);
        if (regionResult == CabinetRegionResult::Changed) {
            if (!backupCabinetSettings(original)) {
                os_notify("Could not back up cabinet settings. Region was not changed.", 10000);
            } else {
                // The BIOS caches country in RAM. Preserve BRAM and teardown
                // before replacing the EEPROM; never interrupt its serial I/O.
                // For a blank chip, wait for the real BIOS defaults, then boot
                // once more with the requested country instead of a fake image.
                if (engine) { releaseGraphics(); ClearAudioOutput(); freshAudioOutput = true; }
                mie->restoreEeprom(std::vector<uint8_t>(updated.begin(), updated.end()));
                os_notify("Cabinet region applied. Calibration and saved records retained.", 5000);
            }
            pendingRegion = CabinetRegion::Saved;
        } else if (regionResult != CabinetRegionResult::PendingInitialization) {
            pendingRegion = CabinetRegion::Saved;
            if (regionResult == CabinetRegionResult::Invalid)
                os_notify("Cabinet settings do not match this game. Region was not changed.", 10000);
        }
    }
    if (!gl && !nativeGraphics) {
        std::unique_ptr<graphics::Backend> backend;
#ifdef USE_METAL
        if (isMetal(requested)) backend = graphics::makeMetalBackend();
#endif
#ifdef USE_VULKAN
        if (isVulkan(requested)) backend = graphics::makeVulkanBackend();
#endif
        nativeGraphics = std::make_unique<graphics::Device>(std::move(backend));
    }
    if (nativeGraphics) nativeGraphics->begin(unsigned(settings.display.width), unsigned(settings.display.height));
    std::optional<graphics::Device::Scope> graphicsScope;
    graphicsScope.emplace(nativeGraphics.get());
    if (!engine) {
        HikaruEngineRoms roms{images->boot.data(), images->boot.size(), images->program.data(), images->program.size(), images->data.data(), images->data.size(), 1};
        auto io = callbacks(); std::array<char, 512> error{};
        auto security = protectionCallbacks();
        const auto game = activeGame == &PlanetHarriersGame ? HIKARU_ENGINE_GAME_PHARRIER
            : activeGame == &BraveFirefightersGame ? HIKARU_ENGINE_GAME_BRAVEFF : HIKARU_ENGINE_GAME_AIRTRIX;
        engine = hikaru_engine_create_game(game, &roms, &io, &security, 0, unsigned(settings.display.width), unsigned(settings.display.height), 1, error.data(), error.size());
        glcache.Reset();
        if (nativeGraphics) nativeGraphics->checkError();
        if (!engine) throw FlycastException(std::string("Hikaru renderer: ") + error.data());
        if (hasSavedBram && hikaru_engine_set_bram(engine, savedBram.data(), savedBram.size()) != 0)
            throw FlycastException("Cannot restore Hikaru battery-backed RAM");
        audioPacer = std::make_unique<AudioPacer>([](void*, s16 left, s16 right) {
            WriteSample(right, left);
        }, nullptr);
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
                    hikaru::Sound::BoardDiagnostics boardDiag[2]{};
                    const bool boardOk0 = sound && sound->diagnostics(0, boardDiag[0]);
                    const bool boardOk1 = sound && sound->diagnostics(1, boardDiag[1]);
                    std::fprintf(stderr, "Hikaru boards: b0 arm=%d pc=%08x keys=%u peak=%u mv=%u status=%08x bank=%08x/%08x cmds=%u bytes=%u | b1 arm=%d pc=%08x keys=%u peak=%u mv=%u status=%08x bank=%08x/%08x cmds=%u bytes=%u\n",
                                 boardOk0 ? int(boardDiag[0].armEnabled) : -1, boardOk0 ? boardDiag[0].armPc : 0,
                                 boardOk0 ? boardDiag[0].keys : 0, boardOk0 ? boardDiag[0].peak : 0, boardOk0 ? boardDiag[0].masterVolume : 0, boardOk0 ? boardDiag[0].status : 0,
                                 boardOk0 ? boardDiag[0].bankReady : 0, boardOk0 ? boardDiag[0].bankRequest : 0,
                                 boardOk0 ? boardDiag[0].queuedCommands : 0, boardOk0 ? boardDiag[0].bankBytes : 0,
                                 boardOk1 ? int(boardDiag[1].armEnabled) : -1, boardOk1 ? boardDiag[1].armPc : 0,
                                 boardOk1 ? boardDiag[1].keys : 0, boardOk1 ? boardDiag[1].peak : 0, boardOk1 ? boardDiag[1].masterVolume : 0, boardOk1 ? boardDiag[1].status : 0,
                                 boardOk1 ? boardDiag[1].bankReady : 0, boardOk1 ? boardDiag[1].bankRequest : 0,
                                 boardOk1 ? boardDiag[1].queuedCommands : 0, boardOk1 ? boardDiag[1].bankBytes : 0);
                    if (boardOk0) std::fprintf(stderr, "Hikaru b0 channels: %s\n", boardDiag[0].channels);
                    if (boardOk1) std::fprintf(stderr, "Hikaru b1 channels: %s\n", boardDiag[1].channels);
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
    if (hikaru_engine_restore_graphics(engine, 0, unsigned(settings.display.width), unsigned(settings.display.height)) != 0)
        throw FlycastException(std::string("Hikaru renderer: ") + hikaru_engine_last_error(engine));
    const bool fastForward = settings.input.fastForwardMode;
    const bool muted = settings.aica.muteAudio;
    const bool pacing = config::HikaruAudioPacing;
    const bool freshOutput = freshAudioOutput.exchange(false);
    if (freshOutput) audioModeKnown = false;
    bool clearedAudio = false;
    if (!audioModeKnown || fastForward != previousFastForward || muted != previousMuted || pacing != previousPacing) {
        audioPacer->reset();
        if (!freshOutput) ClearAudioOutput();
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
    const auto sorting = config::HikaruSorting.get() == config::HikaruSortingMode::PerPixel
        ? HIKARU_TRANSPARENCY_PER_PIXEL
        : config::HikaruSorting.get() == config::HikaruSortingMode::PerStrip
            ? HIKARU_TRANSPARENCY_PER_STRIP : HIKARU_TRANSPARENCY_PER_TRIANGLE;
    if (hikaru_engine_set_transparency_sorting(engine, sorting) != 0)
        throw FlycastException(std::string("Hikaru sorting: ") + hikaru_engine_last_error(engine));
    if (hikaru_engine_set_transparency_peel_capacity(engine, unsigned(std::clamp(config::HikaruMaxLayers.get(), 1, int(HIKARU_MAX_TRANSPARENCY_LAYERS)))) != 0)
        throw FlycastException("Invalid Hikaru per-pixel layer capacity");
    const auto transparencyBudget = config::PixelBufferSize.get();
    if (transparencyBudget <= 0 || hikaru_engine_set_transparency_memory_budget(engine, uint64_t(transparencyBudget)) != 0)
        throw FlycastException("Invalid Hikaru per-pixel memory budget");
    const int requestedHeight = config::RenderResolution.get();
    if (unsigned(requestedHeight) != internalHeight) {
        if (hikaru_engine_set_internal_resolution(engine, unsigned(requestedHeight)) != 0)
            throw FlycastException(std::string("Hikaru renderer: ") + hikaru_engine_last_error(engine));
        internalHeight = unsigned(requestedHeight);
    }
    std::array<ControllerInputs, 2> controllers;
    for (unsigned i = 0; i < controllers.size(); ++i)
        controllers[i] = {kcode[i], joyx[i], joyy[i], lt[i], rt[i]};
    const auto* game = loadedGame();
    auto input = bindCabinetInputs(game, controllers, config::HikaruAirTrixDpadBoard, config::HikaruPlanetDpadStick);
    for (unsigned i = 0; i < credits.size(); ++i)
        input.coin[i] = credits[i].sample(input.coin[i]);
    if (game == &BraveFirefightersGame) {
        if (config::HikaruBraveMouseAim && !gui_is_open()) {
            // Absolute pointer aim, like touch: position inside the
            // picture becomes the full analog range. The last pointer
            // position is held every frame; falling back to the pad
            // value while the pointer rests snapped the hose to the
            // pad's centered axes. Pad/keyboard aim still applies on
            // frames before the pointer first moves.
            static float aimFx = 0.5f, aimFy = 0.5f;
            static s32 lastMouseX = -1, lastMouseY = -1;
            static float wheelAccum = 0.0f;
            const s32 mx = mo_x_abs[0], my = mo_y_abs[0];
            if (mx != lastMouseX || my != lastMouseY) {
                lastMouseX = mx; lastMouseY = my;
                // mo_x_abs/mo_y_abs are native 640x480 picture coords;
                // letterbox offsets were removed when they were stored.
                aimFx = std::clamp(float(mx) / 640.0f, 0.0f, 1.0f);
                aimFy = std::clamp(float(my) / 480.0f, 0.0f, 1.0f);
            }
            // While the pointer is captured for play, SDL delivers
            // relative motion instead of absolute positions, so the
            // absolute values freeze. Integrate the relative deltas
            // (native units) into the held aim; Hikaru has no other
            // consumer for them, so consuming them here is safe.
            {
                std::lock_guard<std::mutex> lock(relPosMutex);
                aimFx = std::clamp(aimFx + mo_x_delta[0] / 640.0f, 0.0f, 1.0f);
                aimFy = std::clamp(aimFy + mo_y_delta[0] / 480.0f, 0.0f, 1.0f);
                mo_x_delta[0] = 0.0f;
                mo_y_delta[0] = 0.0f;
            }
            input.analog[0] = uint16_t(aimFx * 65535.0f);
            input.analog[1] = uint16_t(aimFy * 65535.0f);
            // Buttons, active-low in mo_buttons: left sprays, right is
            // the fog hold (hold right, then spray with left), middle
            // is Start. OR them in so they work regardless of which
            // device is assigned to the port; other bindings unchanged.
            u8 buttons = 0xff;
            for (unsigned port = 0; port < 4; ++port) buttons &= mo_buttons[port];
            if (!(buttons & (1 << 2))) input.players[0] |= 0x0200;
            // Fog hold: set both JVS push-2 positions. The cabinet's
            // second switch byte is unassigned in the Brave mapping,
            // so the extra bit cannot collide with a bound control.
            if (!(buttons & (1 << 1))) input.players[0] |= 0x0100 | 0x0080;
            if (!(buttons & (1 << 3))) input.players[0] |= 0x8000;
            if (traceTiming) {
                static u8 lastButtons = 0xff;
                if (buttons != lastButtons) {
                    std::fprintf(stderr, "Hikaru mouse buttons: raw=%02x players=%04x\n",
                                 unsigned(buttons), unsigned(input.players[0]));
                    lastButtons = buttons;
                }
            }
            wheelAccum += mo_wheel_delta[0];
            mo_wheel_delta[0] = 0.0f;
            while (wheelAccum >= 1.0f) { wheelAccum -= 1.0f; insertCredit(0); }
            while (wheelAccum <= -1.0f) { wheelAccum += 1.0f; }
        }
    }
    GLint target = 0;
    if (!nativeGraphics) glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    hikaru_engine_resize(engine, uint32_t(target), unsigned(settings.display.width), unsigned(settings.display.height));
    const auto engineStart = traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const double blockedStart = traceTiming ? audioOutputBlockedSeconds() : 0;
    int result;
    {
        Sound::FrameScope audioFrame(*sound);
        if (!audioFrame.acquired()) throw FlycastException("Hikaru sound context is already active");
        result = hikaru_engine_run_frame(engine, &input);
    }
    const auto engineEnd = traceTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    glcache.Reset();
    if (nativeGraphics) nativeGraphics->checkError();
    graphicsScope.reset();
    if (nativeGraphics) nativeGraphics->present();
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
        // Native presenters already build and submit the overlay. Submitting
        // it twice would acquire a second Vulkan frame before presentation.
        if (!nativeGraphics) gui_display_osd();
        glcache.Reset();
    }
    if (traceTiming) {
        static unsigned frames;
        static uint64_t frameNumber;
        static double engineMs, overlayMs, totalMs, maximumMs;
        const auto end = std::chrono::steady_clock::now();
        const double frameEngineMs = std::chrono::duration<double, std::milli>(engineEnd - engineStart).count();
        const double frameOverlayMs = std::chrono::duration<double, std::milli>(end - engineEnd).count();
        const double frameTotalMs = std::chrono::duration<double, std::milli>(end - renderStart).count();
        const double blockedMs = std::max(0.0, audioOutputBlockedSeconds() - blockedStart) * 1000;
        ++frameNumber;
        // Window averages hide tutorial-transition hitches. With the existing
        // opt-in trace, separate long frames from time waiting on audio output.
        if (frameTotalMs >= 100)
            std::fprintf(stderr, "Hikaru slow frame: game=%s frame=%llu total_ms=%.3f engine_ms=%.3f audio_wait_ms=%.3f overlay_ms=%.3f\n",
                         game->identifier, static_cast<unsigned long long>(frameNumber),
                         frameTotalMs, frameEngineMs, blockedMs, frameOverlayMs);
        engineMs += frameEngineMs;
        overlayMs += frameOverlayMs;
        totalMs += frameTotalMs;
        maximumMs = std::max(maximumMs, frameTotalMs);
        if (++frames == 60) {
            std::fprintf(stderr, "Hikaru frontend timing: frames=%u engine_ms=%.3f overlay_ms=%.3f total_ms=%.3f max_ms=%.3f cpu_mhz=%u api=%s sorting=%s height=%u\n",
                         frames, engineMs / frames, overlayMs / frames, totalMs / frames, maximumMs, cpuClockMhz,
                         gl ? "opengl" : isMetal(requested) ? "metal" : "vulkan",
                         sorting == HIKARU_TRANSPARENCY_PER_PIXEL ? "pixel" : sorting == HIKARU_TRANSPARENCY_PER_STRIP ? "strip" : "triangle",
                         internalHeight);
            frames = 0; engineMs = overlayMs = totalMs = maximumMs = 0;
        }
    }
    return true;
}
bool render() {
    try { return renderFrame(); }
    catch (const FlycastException&) { throw; }
    catch (const std::exception& e) { throw FlycastException(std::string("Hikaru graphics: ") + e.what()); }
}

}
