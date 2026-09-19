#include "gtest/gtest.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_mem.h"

// Content loading can call initMappings() before flycast_init() has reserved
// the address space. When that happened the fallback buffers were installed,
// a later reserve() then succeeded, and virtmemEnabled() disagreed with where
// mem_b actually lived, tripping the layout check in Sh4Recompiler::Init.
TEST(AddrSpace, MappingsAgreeWithVirtmemWhenNotReservedFirst)
{
	// Drop any reservation a previously run test left behind, so initMappings()
	// is reached in the same state as an early content load.
	addrspace::release();
	ASSERT_FALSE(addrspace::virtmemEnabled());

	addrspace::initMappings();

	// flycast_init() reserves after the fact. That must not leave
	// virtmemEnabled() reporting a layout mem_b is not part of.
	ASSERT_TRUE(addrspace::reserve());

	if (addrspace::virtmemEnabled())
		EXPECT_EQ(&mem_b[0], addrspace::ram_base + 0x0C000000);
}
