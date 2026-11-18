// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace themes::repository
{
struct Theme
{
	std::string id, name, creator, description, version, packageUrl;
	std::vector<std::string> screenshots;
};
using Transport = std::function<int(const std::string&, std::vector<unsigned char>&)>;

// Catalogue URLs and all referenced downloads must use HTTPS.
std::vector<Theme> parse(const std::string& json, const std::string& catalogUrl);
std::vector<Theme> fetch(const std::string& catalogUrl, const Transport& transport = {});
// Worker-safe: publishes a validated package without touching the UI-owned catalogue.
// The caller must call themes::reload() on the UI thread after success.
std::string install(const Theme& theme, const Transport& transport = {});
// Downloads a bounded, validated image to a caller-owned cache file.
void preview(const std::string& url, const std::string& path, const Transport& transport = {});
}
