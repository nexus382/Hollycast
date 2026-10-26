#include "gtest/gtest.h"
#include "ui/readiness.h"

using namespace readiness;

TEST(ReadinessTest, EmptySetupNeedsFolderAndControlsButBiosIsOptional)
{
	const auto result = evaluate({});
	EXPECT_EQ(result.games, Status::Attention);
	EXPECT_EQ(result.bios, Status::Optional);
	EXPECT_EQ(result.controls, Status::Attention);
}

TEST(ReadinessTest, FolderAndControllerWorkWithHleFallback)
{
	Inputs inputs;
	inputs.hasGameFolder = true;
	inputs.hasController = true;
	const auto result = evaluate(inputs);
	EXPECT_EQ(result.games, Status::Ready);
	EXPECT_EQ(result.bios, Status::Optional);
	EXPECT_EQ(result.controls, Status::Ready);
}

TEST(ReadinessTest, RealBiosIsRecognizedWithoutMaskingMissingFolder)
{
	Inputs inputs;
	inputs.hasBios = true;
	const auto result = evaluate(inputs);
	EXPECT_EQ(result.bios, Status::Ready);
	EXPECT_EQ(result.games, Status::Attention);
	EXPECT_EQ(result.controls, Status::Attention);
}

TEST(ReadinessTest, KeyboardAndTouchAreIndependentControlAlternatives)
{
	Inputs inputs;
	inputs.keyboardAvailable = true;
	EXPECT_EQ(evaluate(inputs).controls, Status::Ready);
	inputs.keyboardAvailable = false;
	inputs.touchAvailable = true;
	EXPECT_EQ(evaluate(inputs).controls, Status::Ready);
	inputs.touchAvailable = false;
	EXPECT_EQ(evaluate(inputs).controls, Status::Attention);
}

TEST(ReadinessTest, DisconnectingControllerUpdatesOnlyControls)
{
	Inputs inputs;
	inputs.hasGameFolder = true;
	inputs.hasBios = true;
	inputs.hasController = true;
	EXPECT_EQ(evaluate(inputs).controls, Status::Ready);
	inputs.hasController = false;
	const auto result = evaluate(inputs);
	EXPECT_EQ(result.controls, Status::Attention);
	EXPECT_EQ(result.games, Status::Ready);
	EXPECT_EQ(result.bios, Status::Ready);
}
