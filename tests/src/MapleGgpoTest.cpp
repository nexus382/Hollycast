// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "types.h"
#include "hw/mem/addrspace.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "emulator.h"
#include "cfg/option.h"

// Regression coverage for the GGPO session device profile: a netplay session
// must enumerate a standard pad on port B even when the saved configuration
// leaves it empty, and it must never rewrite the saved configuration.
class MapleGgpoTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		emu.dc_reset(true);
	}

	void setTwoPlayerConfig()
	{
		using namespace config;
		MapleMainDevices[0] = MDT_SegaController;
		MapleExpansionDevices[0][0] = MDT_SegaVMU;
		MapleExpansionDevices[0][1] = MDT_SegaVMU;
		MapleMainDevices[1] = MDT_None;
		MapleExpansionDevices[1][0] = MDT_None;
		MapleExpansionDevices[1][1] = MDT_None;
	}
};

TEST_F(MapleGgpoTest, GgpoSessionAssignsControllerToPortB)
{
	config::Settings::instance().reset();
	setTwoPlayerConfig();
	config::GGPOEnable = true;

	mcfg_CreateDevices();
	u8 deviceTypes[MAPLE_PORTS * 6];
	mcfg_GetDeviceTypes(deviceTypes);
	mcfg_DestroyDevices();

	EXPECT_EQ(deviceTypes[0 * 6 + MAPLE_MAIN_DEV_IDX], MDT_SegaController);
	EXPECT_EQ(deviceTypes[1 * 6 + MAPLE_MAIN_DEV_IDX], MDT_SegaController);
	// The upgrade is session-scoped: the saved configuration is untouched.
	EXPECT_EQ((MapleDeviceType)config::MapleMainDevices[1], MDT_None);
}

TEST_F(MapleGgpoTest, OfflineSessionKeepsPortBEmpty)
{
	config::Settings::instance().reset();
	setTwoPlayerConfig();
	config::GGPOEnable = false;

	mcfg_CreateDevices();
	u8 deviceTypes[MAPLE_PORTS * 6];
	mcfg_GetDeviceTypes(deviceTypes);
	mcfg_DestroyDevices();

	EXPECT_EQ(deviceTypes[1 * 6 + MAPLE_MAIN_DEV_IDX], MDT_None);
}
