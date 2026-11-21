// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <string_view>

namespace discimage {

inline constexpr std::array<std::string_view, 4> extensions{"chd", "gdi", "cue", "cdi"};

inline bool supportsExtension(std::string_view extension)
{
	return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

}
