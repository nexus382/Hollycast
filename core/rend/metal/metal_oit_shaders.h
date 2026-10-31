// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Shared MSL data layout and compute kernels for per-pixel transparency.
extern const char *metalOITShaderSource;
// Append after metalOITShaderSource and metalRegularShaderSource.
extern const char *metalOITCollectShaderSource;
// Append after metalOITShaderSource and metalModifierShaderSource.
extern const char *metalOITModifierShaderSource;
