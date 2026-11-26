#include "gtest/gtest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "cfg/option.h"
#include "input/mapping.h"
#include "stdclass.h"

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
