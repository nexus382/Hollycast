// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <cstddef>
#include <cstdint>

// Check the SFNT directory before passing untrusted theme fonts to FreeType.
inline bool validThemeFontDirectory(const std::uint8_t* data, std::size_t size)
{
	if (size < 12) return false;
	std::size_t offset = 0;
	if (data[0] == 't' && data[1] == 't' && data[2] == 'c' && data[3] == 'f') {
		if (size < 16) return false;
		offset = (std::size_t(data[12]) << 24) | (std::size_t(data[13]) << 16)
				| (std::size_t(data[14]) << 8) | data[15];
	}
	if (offset > size || size - offset < 12) return false;
	const std::uint32_t signature = (std::uint32_t(data[offset]) << 24)
			| (std::uint32_t(data[offset + 1]) << 16) | (std::uint32_t(data[offset + 2]) << 8) | data[offset + 3];
	if (signature != 0x00010000 && signature != 0x4f54544f && signature != 0x74727565) return false;
	const unsigned count = (unsigned(data[offset + 4]) << 8) | data[offset + 5];
	if (count == 0 || count > 256 || count > (size - offset - 12) / 16) return false;
	for (unsigned i = 0; i < count; ++i) {
		const std::size_t entry = offset + 12 + i * 16;
		const std::size_t tableOffset = (std::size_t(data[entry + 8]) << 24) | (std::size_t(data[entry + 9]) << 16)
				| (std::size_t(data[entry + 10]) << 8) | data[entry + 11];
		const std::size_t tableSize = (std::size_t(data[entry + 12]) << 24) | (std::size_t(data[entry + 13]) << 16)
				| (std::size_t(data[entry + 14]) << 8) | data[entry + 15];
		if (tableOffset > size || tableSize > size - tableOffset) return false;
	}
	return true;
}
