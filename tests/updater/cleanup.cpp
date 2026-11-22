// Portions Copyright 2026 The Hollycast Authors
#include "ui/app_update_package.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    try {
        if (argc != 2) throw std::runtime_error("Expected a private test work root");
        const auto root = fs::u8path(argv[1]) / ("cleanup-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        for (const auto* name : {"update-1-2", "update-3-4", "update-5-6", "update-7-8", "update-9-10", "user-folder"}) {
            const auto folder = root / name;
            fs::create_directory(folder);
            std::ofstream(folder / "package.zip") << "payload";
            std::ofstream(folder / "status", std::ios::binary) << (std::string(name) == "update-5-6" ? "Downloading\n" : "Done\n");
        }
        std::ofstream(root / "update-3-4" / "status", std::ios::binary) << "Error\nTest failure";
        std::ofstream(root / "save.vmu") << "keep save";
        std::ofstream(root / "update-9-10" / "user-save.vmu") << "keep user file";
        app_updates::detail::cleanupDownloads(root.u8string(), (root / "update-7-8").u8string());
        if (fs::exists(root / "update-1-2") || fs::exists(root / "update-3-4"))
            throw std::runtime_error("Terminal downloads were retained");
        if (!fs::exists(root / "update-5-6" / "package.zip") || !fs::exists(root / "user-folder" / "package.zip")
                || !fs::exists(root / "save.vmu"))
            throw std::runtime_error("Cleanup touched an active download or user file");
        if (!fs::exists(root / "update-7-8" / "status") || !fs::exists(root / "update-9-10" / "user-save.vmu"))
            throw std::runtime_error("Cleanup consumed a pending result or an unknown file");
        std::cout << "Terminal downloads reclaimed; active work and user files retained\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
