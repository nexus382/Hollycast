#include "gtest/gtest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "input/mapping.h"
#include "stdclass.h"

class InputMappingConfigFileTest : public ::testing::Test
{
protected:
    const std::string fileName = "InputMappingConfigFileTest-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    std::filesystem::path getConfigFilepath() const
    {
        return std::filesystem::u8path(get_writable_config_path("mappings/" + fileName));
    }

    void writeConfigFile(const std::string& contents)
    {
        std::filesystem::create_directories(getConfigFilepath().parent_path());
        std::ofstream configFile(getConfigFilepath());
        configFile << contents;
        configFile.close();
        ASSERT_TRUE(configFile) << getConfigFilepath();
    }

    void TearDown() override
    {
        InputMapping::DeleteMapping(fileName);
    }
};

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
