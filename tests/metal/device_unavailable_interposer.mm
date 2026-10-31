// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Test-only DYLD interposer. Build as a dylib and inject into an isolated app
// process to exercise runtime fallback when no Metal device is available.
#import <Metal/Metal.h>
#include <cstdio>

static id<MTLDevice> noMetalDevice()
{
	std::fputs("[Metal unavailable fixture] device factory returned nil\n", stderr);
	return nil;
}

__attribute__((used)) static const struct
{
	const void *replacement;
	const void *original;
} interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)&noMetalDevice,
	(const void *)&MTLCreateSystemDefaultDevice,
};
