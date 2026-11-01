// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/roms.h"
#include "hw/hikaru/mie.h"
#include "hw/hikaru/protection.h"
#include "engine.h"
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::vector<uint8_t> readFile(const std::filesystem::path& path, uint32_t expectedSize)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    if (file.tellg() != std::streampos(expectedSize))
        throw std::runtime_error("Unexpected file size: " + path.string());
    std::vector<uint8_t> result(expectedSize);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(result.data()), result.size()))
        throw std::runtime_error("Cannot read " + path.string());
    return result;
}
void saveEeprom(const std::filesystem::path& path, const std::array<uint8_t, 128>& contents)
{
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(contents.data()), contents.size());
    file.close();
    std::error_code error;
    if (file) std::filesystem::rename(temporary, path, error);
    if (!file || error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("Cannot save MIE EEPROM: " + path.string());
    }
}
struct Io {
    hikaru::Mie mie;
    hikaru::Protection protection{hikaru::RomImages::AirTrixSecurityKey};
    uint64_t clocks = 0;
    std::string error;
    explicit Io(const std::vector<uint8_t>& rom) : mie(rom) {}
    HikaruEngineProtection protectionCallbacks() {
        return {this,
            [](void* p) { static_cast<Io*>(p)->protection.reset(); },
            [](void* p, unsigned size, uint32_t address, uint64_t* value) -> int {
                auto& io = *static_cast<Io*>(p);
                try { *value = io.protection.read(address, size); return 0; }
                catch (const std::exception& e) { io.error = e.what(); return -1; }
            },
            [](void* p, unsigned size, uint32_t address, uint64_t value) -> int {
                auto& io = *static_cast<Io*>(p);
                try { io.protection.write(address, value, size); return 0; }
                catch (const std::exception& e) { io.error = e.what(); return -1; }
            }};
    }
    HikaruEngineMie callbacks() {
        return {this,
            [](void* p) { auto& io = *static_cast<Io*>(p); io.mie.reset(); io.clocks = 0; },
            [](void* p, unsigned size, uint32_t address, uint64_t* value) -> int {
                auto& io = *static_cast<Io*>(p);
                try { *value = io.mie.readHost(address, size); return 0; }
                catch (const std::exception& e) { io.error = e.what(); return -1; }
            },
            [](void* p, unsigned size, uint32_t address, uint64_t value) -> int {
                auto& io = *static_cast<Io*>(p);
                try { io.mie.writeHost(address, uint32_t(value), size); return 0; }
                catch (const std::exception& e) { io.error = e.what(); return -1; }
            },
            [](void* p, uint32_t cycles) -> int {
                auto& io = *static_cast<Io*>(p);
                // Imported core currently uses a 200 MHz SH4 work clock. The
                // real MIE runs at 16 MHz; preserve fractional ticks per slice.
                try {
                    io.clocks += uint64_t(cycles) * 16;
                    io.mie.advanceTicks(uint32_t(io.clocks / 200));
                    io.clocks %= 200;
                    return 0;
                } catch (const std::exception& e) { io.error = e.what(); return -1; }
            },
            [](void* p, const HikaruEngineInputs* input) {
                hikaru::JvsNode::Inputs value;
                value.test = input->test; value.service = input->service;
                for (unsigned i = 0; i < 2; ++i) { value.players[i] = input->players[i]; value.coin[i] = input->coin[i]; }
                for (unsigned i = 0; i < 8; ++i) value.analog[i] = input->analog[i];
                static_cast<Io*>(p)->mie.setInputs(value);
            },
            nullptr}; // Standalone player does not attach the sound adapter.
    }
};
}

int main(int argc, char** argv)
{
    std::filesystem::path biosPath, romDirectory, miePath, eepromPath;
    uint64_t maxFrames = std::numeric_limits<uint64_t>::max();
    bool bypassEeprom = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--bios" || arg == "--rom-dir" || arg == "--mie" || arg == "--eeprom" || arg == "--frames") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (arg == "--bios") biosPath = value;
            else if (arg == "--rom-dir") romDirectory = value;
            else if (arg == "--mie") miePath = value;
            else if (arg == "--eeprom") eepromPath = value;
            else { try { maxFrames = std::stoull(value); } catch (...) { return 2; } }
        } else if (arg == "--strict-bios") bypassEeprom = false;
        else {
            std::cout << "Hollycast experimental Air Trix player\n"
                "--bios <raw Hikaru BIOS> --rom-dir <Air Trix chip directory> --mie <315-6146.bin> [--eeprom <128-byte settings file>] [--frames N] [--strict-bios]\n"
                "F7: coin; F8: start; arrows: cabinet controls; A/D: yaw; W/S: roll; F5/F6: test/service; Escape: exit\n";
            return arg == "--help" ? 0 : 2;
        }
    }
    if (biosPath.empty() || romDirectory.empty() || miePath.empty()) { std::cerr << "Use --help for required ROM paths\n"; return 2; }
    try {
        auto bios = readFile(biosPath, 0x200000);
        std::string biosName;
        for (const auto& candidate : hikaru::BootRoms)
            if (hikaru::romCrc32(bios) == candidate.crc32) biosName = candidate.name;
        if (biosName.empty()) throw std::runtime_error("Unrecognized Hikaru BIOS CRC32");
        auto images = hikaru::loadAirTrix([&](const hikaru::RomFile& file) {
            return file.name == biosName ? bios : readFile(romDirectory / file.name, file.size);
        }, biosName);
        auto mieRom = readFile(miePath, hikaru::MieBootRom.size);
        hikaru::validateRom(hikaru::MieBootRom, mieRom);
        Io io(mieRom);
        if (!eepromPath.empty() && std::filesystem::exists(eepromPath))
            io.mie.restoreEeprom(readFile(eepromPath, 128));
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) throw std::runtime_error(SDL_GetError());
        struct SdlCleanup { ~SdlCleanup() { SDL_Quit(); } } sdlCleanup;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(SDL_CreateWindow(
            "Hollycast — Air Trix (experimental)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        SDL_GLContext context = SDL_GL_CreateContext(window.get());
        if (!context) throw std::runtime_error(SDL_GetError());
        struct GlCleanup { SDL_GLContext context; ~GlCleanup() { SDL_GL_DeleteContext(context); } } glCleanup{context};
#ifndef __APPLE__
        if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) throw std::runtime_error("Cannot load desktop OpenGL");
#endif
        SDL_GL_SetSwapInterval(1);
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window.get(), &width, &height);
        const HikaruEngineRoms roms{images.boot.data(), images.boot.size(), images.program.data(), images.program.size(), images.data.data(), images.data.size()};
        auto callbacks = io.callbacks();
        auto protectionCallbacks = io.protectionCallbacks();
        std::array<char, 512> error{};
        std::unique_ptr<HikaruEngine, decltype(&hikaru_engine_destroy)> engine(hikaru_engine_create(
            &roms, &callbacks, &protectionCallbacks, 0, unsigned(width), unsigned(height), bypassEeprom, error.data(), error.size()), hikaru_engine_destroy);
        if (!engine) throw std::runtime_error(error.data());
        std::cout << "Air Trix ROM CRCs verified; original MIE ROM loaded. Experimental renderer; audio is not implemented.\n" << std::flush;
        bool running = true;
        int exitCode = 0;
        for (uint64_t frame = 0; running && frame < maxFrames; ++frame) {
            const auto frameStart = std::chrono::steady_clock::now();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) running = false;
            }
            if (!running) break;
            const auto* keys = SDL_GetKeyboardState(nullptr);
            HikaruEngineInputs input{};
            for (auto& axis : input.analog) axis = 0x8000;
            input.test = keys[SDL_SCANCODE_F5]; input.service = keys[SDL_SCANCODE_F6];
            input.coin[0] = keys[SDL_SCANCODE_F7];
            // Air Trix uses button 1/2 for left/right and the JVS directional
            // bits for the front/back foot switches.
            input.players[0] = (keys[SDL_SCANCODE_F8] ? 0x8000 : 0) | (keys[SDL_SCANCODE_LEFT] ? 0x0200 : 0)
                | (keys[SDL_SCANCODE_RIGHT] ? 0x0100 : 0) | (keys[SDL_SCANCODE_UP] ? 0x2000 : 0) | (keys[SDL_SCANCODE_DOWN] ? 0x1000 : 0);
            input.analog[0] = keys[SDL_SCANCODE_A] ? 0x0000 : keys[SDL_SCANCODE_D] ? 0xffff : 0x8000;
            input.analog[1] = keys[SDL_SCANCODE_W] ? 0x0000 : keys[SDL_SCANCODE_S] ? 0xffff : 0x8000;
            SDL_GL_GetDrawableSize(window.get(), &width, &height);
            if (width <= 0 || height <= 0) { SDL_Delay(10); continue; }
            hikaru_engine_resize(engine.get(), 0, unsigned(width), unsigned(height));
            if (hikaru_engine_run_frame(engine.get(), &input)) {
                std::cerr << (io.error.empty() ? hikaru_engine_last_error(engine.get()) : io.error) << '\n';
                exitCode = 1;
                break;
            }
            SDL_GL_SwapWindow(window.get());
            if (frame % 300 == 0) std::cout << "frame=" << frame << " MIE-PC=" << std::hex << io.mie.programCounter()
                << std::dec << " JVS-addressed=" << io.mie.addressed() << '\n' << std::flush;
            std::this_thread::sleep_until(frameStart + std::chrono::microseconds(16667));
        }
        if (!eepromPath.empty()) saveEeprom(eepromPath, io.mie.eepromContents());
        return exitCode;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
