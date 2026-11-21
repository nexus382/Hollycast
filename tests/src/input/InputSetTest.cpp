#include "gtest/gtest.h"

#include "input/mapping.h"

class InputSetTest : public ::testing::Test
{
};

TEST_F(InputSetTest, default_constructor)
{
    // Arrange/Act
    const InputMapping::InputSet defaultValues;

    // Assert
    EXPECT_TRUE(defaultValues.empty());
}

TEST_F(InputSetTest, initializer_constructor_all_unique)
{
    // Arrange
    const InputMapping::InputDef inputDef1{0, InputMapping::InputDef::InputType::BUTTON};
    const InputMapping::InputDef inputDef2{0, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef3{1, InputMapping::InputDef::InputType::AXIS_NEG};
    const InputMapping::InputDef inputDef4{1, InputMapping::InputDef::InputType::BUTTON};
    const InputMapping::InputDef inputDef5{2, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef6{3, InputMapping::InputDef::InputType::AXIS_NEG};

    // Act
    const InputMapping::InputSet inputSet{
        inputDef1, inputDef2, inputDef3, inputDef4, inputDef5, inputDef6
    };

    // Assert
    ASSERT_EQ(inputSet.size(), 6);
    InputMapping::InputSet::const_iterator iter = inputSet.cbegin();
    EXPECT_EQ(*iter++, inputDef1);
    EXPECT_EQ(*iter++, inputDef2);
    EXPECT_EQ(*iter++, inputDef3);
    EXPECT_EQ(*iter++, inputDef4);
    EXPECT_EQ(*iter++, inputDef5);
    EXPECT_EQ(*iter++, inputDef6);
}

TEST_F(InputSetTest, initializer_constructor_duplicate_pos_neg)
{
    // Arrange
    // Can't contain both positive and negative of the same code - wouldn't be physically possible
    const InputMapping::InputDef inputDef1{0, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef2{0, InputMapping::InputDef::InputType::AXIS_NEG};

    // Act
    const InputMapping::InputSet inputSet{
        inputDef1, inputDef2
    };

    // Assert
    ASSERT_EQ(inputSet.size(), 1);
    InputMapping::InputSet::const_iterator iter = inputSet.cbegin();
    EXPECT_EQ(*iter++, inputDef2); // Most recent (last) insertion takes precedence
}

TEST_F(InputSetTest, initializer_constructor_duplicates)
{
    // Arrange
    const InputMapping::InputDef inputDef1{0, InputMapping::InputDef::InputType::BUTTON};
    const InputMapping::InputDef inputDef2{0, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef3{1, InputMapping::InputDef::InputType::AXIS_NEG};
    const InputMapping::InputDef inputDef4{0, InputMapping::InputDef::InputType::BUTTON};
    const InputMapping::InputDef inputDef5{0, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef6{1, InputMapping::InputDef::InputType::AXIS_NEG};

    // Act
    const InputMapping::InputSet inputSet{
        inputDef1, inputDef2, inputDef3, inputDef4, inputDef5, inputDef6
    };

    // Assert
    ASSERT_EQ(inputSet.size(), 3);
    InputMapping::InputSet::const_iterator iter = inputSet.cbegin();
    EXPECT_EQ(*iter++, inputDef1);
    EXPECT_EQ(*iter++, inputDef2);
    EXPECT_EQ(*iter++, inputDef3);
}

TEST_F(InputSetTest, contains)
{
    // Arrange
    const InputMapping::InputDef inputDef1{123, InputMapping::InputDef::InputType::BUTTON};
    const InputMapping::InputDef inputDef2{456, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef3{789, InputMapping::InputDef::InputType::AXIS_NEG};
    const InputMapping::InputDef inputDef4{937547, InputMapping::InputDef::InputType::BUTTON};
    const InputMapping::InputDef inputDef5{8675309, InputMapping::InputDef::InputType::AXIS_POS};
    const InputMapping::InputDef inputDef6{424242424, InputMapping::InputDef::InputType::AXIS_NEG};
    const InputMapping::InputSet inputSet{
        inputDef1, inputDef2, inputDef3, inputDef4, inputDef5, inputDef6
    };

    // Act/Assert
    EXPECT_TRUE(inputSet.contains(inputDef3));
    EXPECT_FALSE(inputSet.contains(InputMapping::InputDef{8675309, InputMapping::InputDef::InputType::BUTTON}));
}

TEST_F(InputSetTest, ends_with_matches_the_last_inputs)
{
    const auto firstButton = InputMapping::InputDef::from_button(1);
    const auto secondButton = InputMapping::InputDef::from_button(2);
    const auto positiveAxis = InputMapping::InputDef::from_axis(1, true);
    const auto negativeAxis = InputMapping::InputDef::from_axis(100, false);
    const InputMapping::InputSet inputs{firstButton, secondButton, positiveAxis, negativeAxis};
    const InputMapping::InputSet ordered{secondButton, positiveAxis, negativeAxis};
    const InputMapping::InputSet reordered{secondButton, negativeAxis, positiveAxis};
    const InputMapping::InputSet withGap{secondButton, negativeAxis};

    EXPECT_TRUE(inputs.ends_with(ordered, true));
    EXPECT_FALSE(inputs.ends_with(reordered, true));
    EXPECT_TRUE(inputs.ends_with(reordered, false));
    EXPECT_FALSE(inputs.ends_with(withGap, false));
}
