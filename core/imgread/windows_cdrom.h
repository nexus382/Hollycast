// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <string>
#include <string_view>

struct Disc;

std::string windowsCdromDevicePath(std::string_view path);
Disc *windows_cdrom_parse(const std::string& devicePath);
