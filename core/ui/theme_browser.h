// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>

namespace themes
{
void openBrowser();
// Finish outstanding downloads before HTTP and application storage shut down.
void stopBrowser();
// Render once per settings frame. Returns a theme id when Use theme is chosen.
std::optional<std::string> renderBrowser();
}
