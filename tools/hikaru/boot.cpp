// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/machine.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
struct Options {
    std::filesystem::path bios;
    std::filesystem::path directory;
    uint64_t steps = 1000000;
    bool verifyOnly = false;
};
void usage()
{
    std::cout << "Usage: hikaru_boot --bios RAW_FILE --rom-dir AIRTRIX_DIRECTORY [--steps N] [--verify-only]\n"
              << "Experimental native machine bring-up; each paired step runs master then slave.\n"
              << "One model tick per instruction is a placeholder, not hardware-accurate timing.\n";
}
std::vector<uint8_t> readFile(const std::filesystem::path& path, uint32_t expectedSize)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        throw std::runtime_error("Cannot inspect ROM file " + path.string() + ": " + error.message());
    if (size != expectedSize)
        throw std::runtime_error(path.string() + ": expected " + std::to_string(expectedSize)
            + " bytes, got " + std::to_string(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open ROM file " + path.string());
    std::vector<uint8_t> data(expectedSize);
    if (!stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read complete ROM file " + path.string());
    return data;
}
Options options(int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verify-only") result.verifyOnly = true;
        else if (arg == "--bios" || arg == "--rom-dir" || arg == "--steps") {
            if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--bios") result.bios = value;
            else if (arg == "--rom-dir") result.directory = value;
            else {
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("--steps requires a nonnegative decimal integer");
                std::size_t end = 0;
                result.steps = std::stoull(value, &end);
                if (end != value.size()) throw std::runtime_error("Invalid --steps value");
            }
        }
        else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (result.bios.empty() || result.directory.empty())
        throw std::runtime_error("Both --bios and --rom-dir are required");
    return result;
}
void printVerified(const hikaru::RomFile& file)
{
    std::cout << "Verified " << file.name << " (" << std::dec << file.size << " bytes, CRC32 "
              << std::hex << std::setw(8) << std::setfill('0') << file.crc32 << std::dec << ")\n";
}
const char* cpuName(hikaru::Machine::Cpu id)
{
    return id == hikaru::Machine::Cpu::Master ? "master" : "slave";
}
} // namespace

int main(int argc, char** argv)
{
    std::unique_ptr<hikaru::Machine> machine;
    try {
        if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            usage();
            return 0;
        }
        const Options args = options(argc, argv);
        const auto bootBytes = readFile(args.bios, hikaru::BoardMemory::BootRomSize);
        const uint32_t bootCrc = hikaru::romCrc32(bootBytes);
        const hikaru::RomFile* selectedBoot = nullptr;
        for (const auto& candidate : hikaru::BootRoms)
            if (candidate.crc32 == bootCrc) selectedBoot = &candidate;
        if (!selectedBoot)
            throw std::runtime_error("Specified BIOS CRC32 is not one of the three supported Hikaru BIOS manifests");
        auto images = hikaru::loadAirTrix([&](const hikaru::RomFile& file) {
            auto data = std::string(file.name) == selectedBoot->name
                ? bootBytes : readFile(args.directory / file.name, file.size);
            hikaru::validateRom(file, data);
            printVerified(file);
            return data;
        }, selectedBoot->name);
        if (args.verifyOnly) {
            std::cout << "Verified BIOS and all 10 Air Trix chips; no CPU instructions executed.\n";
            return 0;
        }
        machine = std::make_unique<hikaru::Machine>();
        machine->loadImages(std::move(images));
        std::cout << "Running " << args.steps << " paired instruction steps (one model tick/instruction).\n" << std::flush;
        machine->runSteps(args.steps);
        std::cout << "Completed requested native instruction budget. Master PC=0x" << std::hex
                  << machine->cpu(hikaru::Machine::Cpu::Master).state().pc << ", slave PC=0x"
                  << machine->cpu(hikaru::Machine::Cpu::Slave).state().pc << std::dec << '\n';
        return 0;
    }
    catch (const hikaru::Machine::Fault& fault) {
        std::cerr << "Native " << cpuName(fault.cpu) << " fault: PC=0x" << std::hex << std::setw(8)
                  << std::setfill('0') << fault.pc << " opcode=0x" << std::setw(4) << fault.opcode
                  << ": " << fault.what() << '\n';
        if (machine) {
            for (auto id : {hikaru::Machine::Cpu::Master, hikaru::Machine::Cpu::Slave}) {
                std::cerr << cpuName(id) << " last " << std::dec << machine->trace(id).size() << " PCs:";
                for (uint32_t pc : machine->trace(id))
                    std::cerr << " " << std::hex << std::setw(8) << pc;
                std::cerr << '\n';
            }
        }
        return 1;
    }
    catch (const std::exception& error) {
        std::cerr << "Hikaru native boot: " << error.what() << '\n';
        return 1;
    }
}
