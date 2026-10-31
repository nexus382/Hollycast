// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Link-time replacement of the device factory exercises the real, cached
// availability checks without changing production behavior or configuration.
#define main context_basic_smoke_main
#include "context_smoke.mm"
#undef main

extern "C" id<MTLDevice> MTLCreateSystemDefaultDevice(void)
{
	return nil;
}

int main()
{
	@autoreleasepool
	{
		require(!metalIsAvailable(), "Metal must be unavailable without a device");
		require(!metalIsAvailable(), "unavailable result remains cached");
		require(!metalSupportsPerPixel(), "per-pixel rendering requires basic Metal availability");
		std::puts("Metal nil-device availability check passed");
	}
	return 0;
}
