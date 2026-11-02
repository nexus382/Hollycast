// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace hikaru::graphics {
std::string metalShaderSource();
std::string vulkanVertexSource(bool bitmap);
std::string vulkanFragmentSource(bool bitmap);
}
