#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "cfg/option.h"
#include "input/mapping.h"
#include "oslib/storage.h"
#include "stdclass.h"

namespace mapping_internal
{
bool createLegacyBackup(hostfs::Storage& storage, const std::string& sourcePath);
}

namespace
{

enum class BackupFailure { None, ShortWrite, Flush, Close, ExistingBackupRead };

struct BackupProviderState
{
    std::map<std::string, std::string> files;
    BackupFailure failure = BackupFailure::None;
    int temporaryNumber = 0;
    int createCalls = 0;
    int replaceCalls = 0;
    std::string destination;
    std::string parentReference;
    std::string siblingName;
    std::string backupDuringCopy;
};

class BackupProviderFile : public hostfs::File
{
public:
    BackupProviderFile(std::shared_ptr<BackupProviderState> state, std::string path, bool writable)
        : state(std::move(state)), path(std::move(path)), writable(writable) {}
    ~BackupProviderFile() override { close(); }

    size_t read(void *buffer, size_t size, size_t count) override
    {
        if (state->failure == BackupFailure::ExistingBackupRead && path == backupPath)
        {
            failed = true;
            return 0;
        }
        const std::string& contents = state->files.at(path);
        const size_t requested = size * count;
        const size_t available = position < contents.size() ? contents.size() - position : 0;
        const size_t amount = std::min(requested, available);
        if (amount != 0)
            std::memcpy(buffer, contents.data() + position, amount);
        position += amount;
        return size == 0 ? 0 : amount / size;
    }

    size_t write(const void *buffer, size_t size, size_t count) override
    {
        size_t amount = size * count;
        if (state->failure == BackupFailure::ShortWrite && amount != 0)
            --amount;
        std::string& contents = state->files[path];
        if (position + amount > contents.size())
            contents.resize(position + amount);
        if (amount != 0)
            std::memcpy(contents.data() + position, buffer, amount);
        position += amount;
        return size == 0 ? 0 : amount / size;
    }

    s64 tell() override { return position; }
    int seek(s64 offset, int whence) override
    {
        const s64 target = whence == SEEK_SET ? offset : whence == SEEK_CUR
            ? static_cast<s64>(position) + offset : static_cast<s64>(state->files[path].size()) + offset;
        if (target < 0)
            return -1;
        position = static_cast<size_t>(target);
        return 0;
    }
    char *gets(char *, int) override { return nullptr; }
    s64 size() override { return state->files[path].size(); }
    int eof() override { return !failed && position >= state->files[path].size(); }
    int error() override { return failed; }
    bool flush() override { return state->failure != BackupFailure::Flush; }
    bool close() override
    {
        if (closed)
            return true;
        closed = true;
        return !writable || state->failure != BackupFailure::Close;
    }

    static const std::string backupPath;

private:
    std::shared_ptr<BackupProviderState> state;
    std::string path;
    size_t position = 0;
    bool writable;
    bool failed = false;
    bool closed = false;
};

const std::string BackupProviderFile::backupPath = "provider://folder/opaque-backup-id";

class BackupProvider : public hostfs::Storage
{
public:
    BackupProvider() : state(std::make_shared<BackupProviderState>()) {}
    bool isKnownPath(const std::string&) override { return true; }
    std::vector<hostfs::FileInfo> listContent(const std::string&) override { return {}; }
    hostfs::File *openFile(const std::string& path, const std::string& mode) override
    {
        if (mode[0] == 'r' && state->files.count(path) == 0)
            return nullptr;
        return new BackupProviderFile(state, path, mode[0] != 'r');
    }
    std::string getParentPath(const std::string&) override { return "provider://folder/opaque-parent-id"; }
    std::string getSubPath(const std::string& parent, const std::string& child) override
    {
        state->parentReference = parent;
        state->siblingName = child;
        return BackupProviderFile::backupPath;
    }
    hostfs::FileInfo getFileInfo(const std::string& path) override
    {
        if (path == sourcePath)
            return {"legacy.ini", path, false, state->files[path].size()};
        auto found = state->files.find(path);
        if (found == state->files.end())
            throw hostfs::StorageException("missing file");
        return {"backup", path, false, found->second.size()};
    }
    bool exists(const std::string& path) override { return state->files.count(path) != 0; }
    hostfs::File *createTemporaryFile(const std::string& destination, std::string& temporary) override
    {
        ++state->createCalls;
        if (!state->backupDuringCopy.empty())
            state->files[destination] = state->backupDuringCopy;
        state->destination = destination;
        temporary = "provider://opaque-temporary-id/" + std::to_string(++state->temporaryNumber);
        state->files[temporary].clear();
        return new BackupProviderFile(state, temporary, true);
    }
    bool replaceFile(const std::string& temporary, const std::string& destination) override
    {
        ++state->replaceCalls;
        auto found = state->files.find(temporary);
        if (found == state->files.end())
            return false;
        state->files[destination] = std::move(found->second);
        state->files.erase(found);
        return true;
    }
    bool removeFile(const std::string& path) override
    {
        state->files.erase(path);
        return true;
    }

    static const std::string sourcePath;
    std::shared_ptr<BackupProviderState> state;
};

const std::string BackupProvider::sourcePath = "provider://document/opaque-source-id";

} // namespace

TEST(InputMappingBackupProviderTest, StreamsAndRetriesOpaqueSiblingPublicationFailures)
{
    const std::string source(5001, 'x');
    for (BackupFailure failure : {BackupFailure::ShortWrite, BackupFailure::Flush, BackupFailure::Close})
    {
        BackupProvider storage;
        storage.state->files[BackupProvider::sourcePath] = source;
        storage.state->failure = failure;

        EXPECT_FALSE(mapping_internal::createLegacyBackup(storage, BackupProvider::sourcePath));
        EXPECT_EQ(storage.state->files.at(BackupProvider::sourcePath), source);
        EXPECT_TRUE(storage.state->files.count(BackupProviderFile::backupPath) == 0);
        EXPECT_EQ(storage.state->destination, BackupProviderFile::backupPath);
        EXPECT_EQ(storage.state->parentReference, "provider://folder/opaque-parent-id");
        EXPECT_EQ(storage.state->siblingName, "legacy.ini.save");
        EXPECT_EQ(storage.state->replaceCalls, 0);

        storage.state->failure = BackupFailure::None;
        EXPECT_TRUE(mapping_internal::createLegacyBackup(storage, BackupProvider::sourcePath));
        EXPECT_EQ(storage.state->files.at(BackupProvider::sourcePath), source);
        EXPECT_EQ(storage.state->files.at(BackupProviderFile::backupPath), source);
        EXPECT_EQ(storage.state->destination, BackupProviderFile::backupPath);
        EXPECT_EQ(storage.state->replaceCalls, 1);
    }
}

TEST(InputMappingBackupProviderTest, PreservesUnreadableExistingBackupAndRetriesProbe)
{
    BackupProvider storage;
    const std::string source = "legacy source";
    const std::string historical = "previous backup bytes";
    storage.state->files[BackupProvider::sourcePath] = source;
    storage.state->files[BackupProviderFile::backupPath] = historical;
    storage.state->failure = BackupFailure::ExistingBackupRead;

    EXPECT_FALSE(mapping_internal::createLegacyBackup(storage, BackupProvider::sourcePath));
    EXPECT_EQ(storage.state->files.at(BackupProviderFile::backupPath), historical);
    EXPECT_EQ(storage.state->createCalls, 0);
    EXPECT_EQ(storage.state->replaceCalls, 0);

    storage.state->failure = BackupFailure::None;
    EXPECT_TRUE(mapping_internal::createLegacyBackup(storage, BackupProvider::sourcePath));
    EXPECT_EQ(storage.state->files.at(BackupProviderFile::backupPath), historical);
    EXPECT_EQ(storage.state->createCalls, 0);
}

TEST(InputMappingBackupProviderTest, RetainsBackupThatAppearsWhileCopying)
{
    BackupProvider storage;
    storage.state->files[BackupProvider::sourcePath] = "legacy source";
    storage.state->backupDuringCopy = "earlier button bindings";
    EXPECT_TRUE(mapping_internal::createLegacyBackup(storage, BackupProvider::sourcePath));
    EXPECT_EQ(storage.state->files.at(BackupProviderFile::backupPath), storage.state->backupDuringCopy);
    EXPECT_EQ(storage.state->replaceCalls, 0);
}

class InputMappingConfigFileTest : public ::testing::Test
{
protected:
    const std::string fileName = "InputMappingConfigFileTest-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::vector<std::string> originalMappingPaths;
    std::vector<std::string> profileNames;
    std::filesystem::path customRoot;

    void SetUp() override
    {
        originalMappingPaths = config::MappingsPath.get();
        customRoot = std::filesystem::temp_directory_path() / (fileName + "-custom");
    }

    std::filesystem::path getConfigFilepath() const
    {
        return getDefaultProfilePath(fileName);
    }

    std::filesystem::path getDefaultProfilePath(const std::string& name) const
    {
        return std::filesystem::u8path(get_writable_config_path("mappings/" + name));
    }

    std::filesystem::path getCustomProfilePath(const std::string& name) const
    {
        return customRoot / name;
    }

    void trackProfile(const std::string& name)
    {
        profileNames.push_back(name);
    }

    void writeRawFile(const std::filesystem::path& path, const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.close();
        ASSERT_TRUE(file) << path;
    }

    std::string readRawFile(const std::filesystem::path& path) const
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    void writeConfigFile(const std::string& contents)
    {
        writeRawFile(getConfigFilepath(), contents);
    }

    void TearDown() override
    {
        InputMapping::ClearMappings();
        config::MappingsPath = originalMappingPaths;
        for (const std::string& name : profileNames)
        {
            std::error_code error;
            std::filesystem::remove(getDefaultProfilePath(name), error);
            std::filesystem::remove(getDefaultProfilePath(name).string() + ".save", error);
            std::filesystem::remove(getDefaultProfilePath(name).string() + ".save.state", error);
            std::filesystem::remove(getCustomProfilePath(name), error);
            std::filesystem::remove(getCustomProfilePath(name).string() + ".save", error);
            std::filesystem::remove(getCustomProfilePath(name).string() + ".save.state", error);
        }
        {
            std::error_code error;
            std::filesystem::remove(getConfigFilepath(), error);
            std::filesystem::remove(getConfigFilepath().string() + ".save", error);
        }
        std::error_code error;
        std::filesystem::remove_all(customRoot, error);
    }
};

TEST_F(InputMappingConfigFileTest, LegacyBackupPreservesOriginalBytes)
{
    const std::string legacyV1Name = fileName;
    trackProfile(legacyV1Name);
    config::MappingsPath.set({});

    std::string legacyV1 = "; Legacy controller profile\r\n[emulator]\r\nversion = 1\r\n";
    for (int i = 0; i < 60; ++i)
        legacyV1 += "; padding for multi-chunk copy: 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n";
    legacyV1 += "[dreamcast]\r\nbtn_a = 100"; // no trailing newline
    ASSERT_GT(legacyV1.size(), 4096u);
    writeRawFile(getDefaultProfilePath(legacyV1Name), legacyV1);
    writeRawFile(getDefaultProfilePath(legacyV1Name).string() + ".save", "");

    InputMapping::ClearMappings();
    auto v1Mapping = InputMapping::LoadMapping(legacyV1Name);
    ASSERT_NE(v1Mapping, nullptr);
    EXPECT_TRUE(v1Mapping->is_dirty());
    EXPECT_EQ(v1Mapping->get_button_code(0, DreamcastKey::DC_BTN_A), 100u);
    EXPECT_EQ(readRawFile(getDefaultProfilePath(legacyV1Name).string() + ".save"), legacyV1);
    EXPECT_EQ(InputMapping::LoadMapping(legacyV1Name), v1Mapping);

    ASSERT_TRUE(InputMapping::SaveMapping(legacyV1Name, v1Mapping));
    EXPECT_FALSE(v1Mapping->is_dirty());
    EXPECT_NE(readRawFile(getDefaultProfilePath(legacyV1Name)).find("version = 4"), std::string::npos);
    EXPECT_EQ(readRawFile(getDefaultProfilePath(legacyV1Name).string() + ".save"), legacyV1);
    InputMapping::ClearMappings();
    auto upgradedV1Mapping = InputMapping::LoadMapping(legacyV1Name);
    ASSERT_NE(upgradedV1Mapping, nullptr);
    EXPECT_FALSE(upgradedV1Mapping->is_dirty());
    EXPECT_EQ(readRawFile(getDefaultProfilePath(legacyV1Name).string() + ".save"), legacyV1);

    const std::string legacyV2Name = fileName + "-v2";
    trackProfile(legacyV2Name);
    config::MappingsPath.set({customRoot.string()});
    const std::string legacyV2 = "; Custom legacy profile\r\n[emulator]\r\nversion = 2\r\n"
        "[dreamcast]\r\nbtn_b = 200";
    writeRawFile(getCustomProfilePath(legacyV2Name), legacyV2);

    InputMapping::ClearMappings();
    auto v2Mapping = InputMapping::LoadMapping(legacyV2Name);
    ASSERT_NE(v2Mapping, nullptr);
    EXPECT_TRUE(v2Mapping->is_dirty());
    EXPECT_EQ(v2Mapping->get_button_code(0, DreamcastKey::DC_BTN_B), 200u);
    EXPECT_EQ(readRawFile(getCustomProfilePath(legacyV2Name).string() + ".save"), legacyV2);
    ASSERT_TRUE(InputMapping::SaveMapping(legacyV2Name, v2Mapping));
    EXPECT_EQ(readRawFile(getCustomProfilePath(legacyV2Name).string() + ".save"), legacyV2);

    const std::string failedBackupName = fileName + "-failed-backup";
    trackProfile(failedBackupName);
    const std::string failedBackupSource = "[emulator]\r\nversion = 1\r\n[dreamcast]\r\nbtn_a = 321";
    writeRawFile(getCustomProfilePath(failedBackupName), failedBackupSource);
    const std::filesystem::path blockedBackup = getCustomProfilePath(failedBackupName).string() + ".save";
    std::filesystem::create_directories(blockedBackup);
    writeRawFile(blockedBackup / "marker", "keep");

    InputMapping::ClearMappings();
    auto failedBackupMapping = InputMapping::LoadMapping(failedBackupName);
    ASSERT_NE(failedBackupMapping, nullptr);
    EXPECT_TRUE(failedBackupMapping->is_dirty());
    EXPECT_EQ(failedBackupMapping->get_button_code(0, DreamcastKey::DC_BTN_A), 321u);
    EXPECT_EQ(readRawFile(getCustomProfilePath(failedBackupName)), failedBackupSource);
    EXPECT_EQ(readRawFile(blockedBackup / "marker"), "keep");
    std::filesystem::remove_all(blockedBackup);
    InputMapping::ClearMappings();
    failedBackupMapping = InputMapping::LoadMapping(failedBackupName);
    ASSERT_NE(failedBackupMapping, nullptr);
    EXPECT_EQ(readRawFile(blockedBackup), failedBackupSource);
    EXPECT_EQ(readRawFile(getCustomProfilePath(failedBackupName)), failedBackupSource);

    const std::string existingBackupName = fileName + "-existing-backup";
    trackProfile(existingBackupName);
    const std::string existingBackupSource = "[emulator]\r\nversion = 2\r\n[dreamcast]\r\nbtn_a = 654";
    const std::string historicalBackup = "[emulator]\r\nversion = 1\r\n[dreamcast]\r\nbtn_b = 987\r\n";
    writeRawFile(getCustomProfilePath(existingBackupName), existingBackupSource);
    writeRawFile(getCustomProfilePath(existingBackupName).string() + ".save", historicalBackup);

    InputMapping::ClearMappings();
    auto existingBackupMapping = InputMapping::LoadMapping(existingBackupName);
    ASSERT_NE(existingBackupMapping, nullptr);
    EXPECT_TRUE(existingBackupMapping->is_dirty());
    EXPECT_EQ(readRawFile(getCustomProfilePath(existingBackupName).string() + ".save"), historicalBackup);

    const std::string currentName = fileName + "-v4";
    trackProfile(currentName);
    config::MappingsPath.set({});
    const std::string currentProfile = "[digital]\nbind0 = 777:btn_a\n[emulator]\nversion = 4";
    writeRawFile(getDefaultProfilePath(currentName), currentProfile);

    InputMapping::ClearMappings();
    auto currentMapping = InputMapping::LoadMapping(currentName);
    ASSERT_NE(currentMapping, nullptr);
    EXPECT_FALSE(currentMapping->is_dirty());
    EXPECT_EQ(currentMapping->get_button_code(0, DreamcastKey::DC_BTN_A), 777u);
    EXPECT_FALSE(std::filesystem::exists(getDefaultProfilePath(currentName).string() + ".save"));
    EXPECT_EQ(InputMapping::LoadMapping(currentName), currentMapping);
}

TEST_F(InputMappingConfigFileTest, save)
{
    // Arrange
    InputMapping inputMapping;
    const DreamcastKey mappedKey1 = DreamcastKey::EMU_BTN_MENU;
    const InputMapping::ButtonCombo combo1{
        InputMapping::InputSet{
            InputMapping::InputDef{191, InputMapping::InputDef::InputType::AXIS_NEG},
            InputMapping::InputDef{123, InputMapping::InputDef::InputType::BUTTON},
            InputMapping::InputDef{987, InputMapping::InputDef::InputType::AXIS_POS}
        },
        true
    };
    inputMapping.set_button(mappedKey1, combo1);
    const DreamcastKey mappedKey2 = DreamcastKey::EMU_BTN_SAVESTATE;
    const InputMapping::ButtonCombo combo2{
        InputMapping::InputSet{
            InputMapping::InputDef{321, InputMapping::InputDef::InputType::BUTTON},
            InputMapping::InputDef{789, InputMapping::InputDef::InputType::AXIS_POS},
            InputMapping::InputDef{1, InputMapping::InputDef::InputType::AXIS_NEG}
        },
        false
    };
    inputMapping.set_button(mappedKey2, combo2);
    const DreamcastKey mappedKey3 = DreamcastKey::EMU_BTN_LOADSTATE;
    const InputMapping::ButtonCombo combo3{
        InputMapping::InputSet{
            InputMapping::InputDef{987, InputMapping::InputDef::InputType::AXIS_POS},
            InputMapping::InputDef{123, InputMapping::InputDef::InputType::BUTTON},
            InputMapping::InputDef{191, InputMapping::InputDef::InputType::AXIS_NEG}
        },
        true
    };
    inputMapping.set_button(mappedKey3, combo3);
    inputMapping.set_button(DreamcastKey::DC_BTN_A, 100);
    inputMapping.set_button(DreamcastKey::DC_BTN_B, 200);
    inputMapping.set_axis(DreamcastKey::DC_AXIS_LT, 1, true);
    inputMapping.set_axis(DreamcastKey::DC_AXIS_RT, 2, true);
    inputMapping.set_axis(DreamcastKey::DC_AXIS_UP, 3, true);
    inputMapping.set_axis(DreamcastKey::DC_AXIS_DOWN, 3, false);
    inputMapping.name = "save test case";
    inputMapping.dead_zone = 0.33;
    inputMapping.rumblePower = 98;
    inputMapping.saturation = 0.50;

    // Act
    ASSERT_TRUE(inputMapping.save(fileName));

    // Assert
    std::ifstream configFile(getConfigFilepath());
    std::ostringstream buffer;
    buffer << configFile.rdbuf();
    ASSERT_EQ(buffer.str(), R"([analog]
bind0 = 1+:btn_trigger_left
bind1 = 2+:btn_trigger_right
bind2 = 3-:btn_analog_down
bind3 = 3+:btn_analog_up

[combo]
bind0 = 321,789+,1-:btn_quick_save:0
bind1 = 987+,123,191-:btn_jump_state:1
bind2 = 191-,123,987+:btn_menu:1

[digital]
bind0 = 100:btn_a
bind1 = 200:btn_b

[emulator]
dead_zone = 33
mapping_name = save test case
rumble_power = 98
saturation = 50
triggers = 
version = 4

)");
}

TEST_F(InputMappingConfigFileTest, load)
{
    // Arrange
    const std::string fileData = R"([analog]
bind0 = 1+:btn_trigger_left
bind1 = 2+:btn_trigger_right
bind2 = 3-:btn_analog_down
bind3 = 3+:btn_analog_up

[combo]
bind0 = 321,789+,1-:btn_quick_save:0
bind1 = 987+,123,191-:btn_jump_state:1
bind2 = 191-,123,987+:btn_menu:1

[digital]
bind0 = 100:btn_a
bind1 = 200:btn_b

[emulator]
dead_zone = 33
mapping_name = save test case
rumble_power = 98
saturation = 50
version = 4)";
    writeConfigFile(fileData);

    // Act
    std::shared_ptr<InputMapping> inputMapping = InputMapping::LoadMapping(fileName);

    // Assert
    ASSERT_NE(inputMapping, nullptr);
    const DreamcastKey mappedKey1 = DreamcastKey::EMU_BTN_MENU;
    const InputMapping::ButtonCombo combo1{
        InputMapping::InputSet{
            InputMapping::InputDef{191, InputMapping::InputDef::InputType::AXIS_NEG},
            InputMapping::InputDef{123, InputMapping::InputDef::InputType::BUTTON},
            InputMapping::InputDef{987, InputMapping::InputDef::InputType::AXIS_POS}
        },
        true
    };
    EXPECT_EQ(inputMapping->get_button_combo(0, mappedKey1), combo1);
    const DreamcastKey mappedKey2 = DreamcastKey::EMU_BTN_SAVESTATE;
    const InputMapping::ButtonCombo combo2{
        InputMapping::InputSet{
            InputMapping::InputDef{321, InputMapping::InputDef::InputType::BUTTON},
            InputMapping::InputDef{789, InputMapping::InputDef::InputType::AXIS_POS},
            InputMapping::InputDef{1, InputMapping::InputDef::InputType::AXIS_NEG}
        },
        false
    };
    EXPECT_EQ(inputMapping->get_button_combo(0, mappedKey2), combo2);
    const DreamcastKey mappedKey3 = DreamcastKey::EMU_BTN_LOADSTATE;
    const InputMapping::ButtonCombo combo3{
        InputMapping::InputSet{
            InputMapping::InputDef{987, InputMapping::InputDef::InputType::AXIS_POS},
            InputMapping::InputDef{123, InputMapping::InputDef::InputType::BUTTON},
            InputMapping::InputDef{191, InputMapping::InputDef::InputType::AXIS_NEG}
        },
        true
    };
    EXPECT_EQ(inputMapping->get_button_combo(0, mappedKey3), combo3);
    EXPECT_EQ(inputMapping->get_button_code(0, DreamcastKey::DC_BTN_A), 100);
    EXPECT_EQ(inputMapping->get_button_code(0, DreamcastKey::DC_BTN_B), 200);
    EXPECT_EQ(inputMapping->get_axis_code(0, DreamcastKey::DC_AXIS_LT), (std::pair<u32,bool>(1, true)));
    EXPECT_EQ(inputMapping->get_axis_code(0, DreamcastKey::DC_AXIS_RT), (std::pair<u32,bool>(2, true)));
    EXPECT_EQ(inputMapping->get_axis_code(0, DreamcastKey::DC_AXIS_UP), (std::pair<u32,bool>(3, true)));
    EXPECT_EQ(inputMapping->get_axis_code(0, DreamcastKey::DC_AXIS_DOWN), (std::pair<u32,bool>(3, false)));
    EXPECT_EQ(inputMapping->name, "save test case");
    EXPECT_NEAR(inputMapping->dead_zone, 0.33, 1e-3);
    EXPECT_EQ(inputMapping->rumblePower, 98);
    EXPECT_NEAR(inputMapping->saturation, 0.50, 1e-3);
}

TEST_F(InputMappingConfigFileTest, clear_makes_dirty)
{
    writeConfigFile("[emulator]\nversion = 4\n"
                    "[digital]\nbind0 = 100:btn_a\n"
                    "[analog]\nbind0 = 3+:btn_analog_up\n");
    auto mapping = InputMapping::LoadMapping(fileName);
    ASSERT_NE(mapping, nullptr);
    ASSERT_FALSE(mapping->is_dirty());
    ASSERT_EQ(100u, mapping->get_button_code(0, DC_BTN_A));

    mapping->clear_button(0, DC_BTN_A);
    EXPECT_EQ(InputMapping::InputDef::INVALID_CODE, mapping->get_button_code(0, DC_BTN_A));
    EXPECT_TRUE(mapping->is_dirty());

    ASSERT_TRUE(mapping->save(fileName));
    ASSERT_FALSE(mapping->is_dirty());
    ASSERT_EQ(3u, mapping->get_axis_code(0, DC_AXIS_UP).first);
    mapping->clear_axis(0, DC_AXIS_UP);
    EXPECT_EQ(InputMapping::InputDef::INVALID_CODE, mapping->get_axis_code(0, DC_AXIS_UP).first);
    EXPECT_TRUE(mapping->is_dirty());
}
