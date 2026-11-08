// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace texture_atlas
{
constexpr const char *MappingFilename = "texture-atlas.json";

struct Region
{
	uint32_t x, y, width, height;
	bool operator==(const Region& other) const;
};

struct Sheet
{
	uint32_t hash, width, height;
	std::vector<Region> regions;
};

// Coordinates are in the source sheet, before renderer-specific UV adjustments.
std::optional<Region> quadRegion(const std::array<std::array<float, 2>, 4>& uv,
		uint32_t width, uint32_t height);
bool addRegion(Sheet& sheet, const Region& region);
std::string imageFilename(uint32_t hash);
std::string regionFilename(uint32_t hash, const Region& region);
std::vector<Sheet> parseMapping(const std::string& text);
std::string writeMapping(const std::vector<Sheet>& sheets);

struct Image
{
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> pixels;
};

// All images and mapping rectangles use top-left image coordinates.
Image rebuild(const Sheet& sheet, const Image& original,
		const std::vector<std::optional<Image>>& tiles, uint32_t maxWidth, uint32_t maxHeight);
}
