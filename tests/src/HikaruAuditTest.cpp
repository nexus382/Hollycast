// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gtest/gtest.h"
#include "hw/hikaru/nvram.h"
#include "types.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

TEST(HikaruRendererTest, PlatformSwitchRestoresEachRequestedOitBackend)
{
    for (auto api : {RenderType::OpenGL_OIT, RenderType::Vulkan_OIT, RenderType::Metal_OIT, RenderType::DirectX11_OIT}) {
        const auto host = hostRendererType(api, true);
        EXPECT_FALSE(isPerPixel(host));
        EXPECT_EQ(isOpenGL(host), isOpenGL(api));
        EXPECT_EQ(isVulkan(host), isVulkan(api));
        EXPECT_EQ(isMetal(host), isMetal(api));
        EXPECT_EQ(host == RenderType::DirectX11, api == RenderType::DirectX11_OIT);
        EXPECT_EQ(hostRendererType(api, false), api);
        EXPECT_EQ(hostRendererType(host, true), host);
    }
}

class HikaruNvramTest : public ::testing::Test {
protected:
    std::filesystem::path directory;
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("hollycast-nvram-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
};

TEST_F(HikaruNvramTest, PublishesCompleteSaveWhileAnotherWriterOwnsAStagingFile)
{
    const auto file = directory / std::filesystem::u8path(u8"cabinet-\u65e5\u672c.eeprom");
    const auto utf8 = file.u8string();
    const std::string path(utf8.begin(), utf8.end());
    auto occupied = file;
    occupied += ".tmp.0";
    std::ofstream(occupied, std::ios::binary) << "other writer";
    const std::array<uint8_t, 4> first{{1,2,3,4}}, next{{4,3,2,1}};
    ASSERT_TRUE(hikaru::saveNvram(path, first));
    ASSERT_TRUE(hikaru::saveNvram(path, next));
    std::array<uint8_t, 4> loaded{};
    ASSERT_EQ(hikaru::loadNvram(path, loaded), hikaru::NvramLoadResult::Loaded);
    EXPECT_EQ(loaded, next);
    std::ifstream pending(occupied, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(pending), {}), "other writer");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), {}), 2);
}

TEST_F(HikaruNvramTest, BackupPreservesTheFirstSnapshotAndSupportsUnicodePaths)
{
    const auto file = directory / std::filesystem::u8path(u8"cabinet-\u65e5\u672c.before-region");
    const auto utf8 = file.u8string();
    const std::string path(utf8.begin(), utf8.end());
    const std::array<uint8_t, 4> original{{1,2,3,4}}, changed{{9,8,7,6}};
    ASSERT_TRUE(hikaru::backupNvram(path, original));
    ASSERT_TRUE(hikaru::backupNvram(path, changed));
    std::array<uint8_t, 4> loaded{};
    ASSERT_EQ(hikaru::loadNvram(path, loaded), hikaru::NvramLoadResult::Loaded);
    EXPECT_EQ(loaded, original);
}

TEST_F(HikaruNvramTest, BackupDoesNotOverwriteAnInvalidExistingSnapshot)
{
    const auto file = directory / "cabinet.before-region";
    { std::ofstream output(file, std::ios::binary); output << "bad"; }
    const std::array<uint8_t, 4> data{{1,2,3,4}};
    EXPECT_FALSE(hikaru::backupNvram(file.string(), data));
    std::ifstream input(file, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "bad");
}

TEST_F(HikaruNvramTest, RejectsTruncatedAndOversizedDataWithoutChangingDefaultsOrFile)
{
    const auto file = directory / "save.nvmem";
    const auto utf8 = file.u8string();
    const std::string path(utf8.begin(), utf8.end());
    std::array<uint8_t, 4> data{{9,9,9,9}};
    EXPECT_EQ(hikaru::loadNvram(path, data), hikaru::NvramLoadResult::Missing);
    for (const std::string bytes : {std::string("ab"), std::string("abcde")}) {
        { std::ofstream output(file, std::ios::binary); output << bytes; }
        EXPECT_EQ(hikaru::loadNvram(path, data), hikaru::NvramLoadResult::Invalid);
        EXPECT_EQ(data, (std::array<uint8_t,4>{{9,9,9,9}}));
        EXPECT_EQ(std::filesystem::file_size(file), bytes.size());
        std::ifstream input(file, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), bytes);
    }
    { std::ofstream output(file, std::ios::binary); output << "abcd"; }
    EXPECT_EQ(hikaru::loadNvram(path, data), hikaru::NvramLoadResult::Loaded);
    EXPECT_EQ(data, (std::array<uint8_t,4>{{'a','b','c','d'}}));
}
