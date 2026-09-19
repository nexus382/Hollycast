// Bench-fixture registration. Linked only with the matching aot_sh4_*.S
// (arm64-only). Not a title corpus. Gameplay fps stays on GameBootBench.

#include "hw/sh4/dyna/aot_table.h"

#if FEAT_SHREC == DYNAREC_IR

extern "C" int aot_sh4_0000beef_1083127669(void *sh4ctx);

static const AotTableEntry kBenchFixture[] = {
	{ 0x0000beefu, 0u, 1083127669u, 6u, &aot_sh4_0000beef_1083127669 },
};

static void aotRegisterBenchFixture() __attribute__((constructor));
static void aotRegisterBenchFixture()
{
	aotSetTableForTest(kBenchFixture, 1);
}

#endif
