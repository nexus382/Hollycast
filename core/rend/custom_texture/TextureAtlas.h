// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
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

class RegionDetector
{
public:
	RegionDetector(uint32_t width, uint32_t height) : width(width), height(height) {}
	// Match source-space triangle halves across strips, before renderer UV adjustments.
	std::optional<Region> submitTriangle(const std::array<std::array<float, 2>, 3>& uv);

private:
	uint32_t width, height;
	std::map<std::array<uint32_t, 4>, uint16_t> triangles;
};
// Append a unique rectangle without changing previously exported crop identities.
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

std::pair<uint32_t, uint32_t> scaledDimensions(const Sheet& sheet, uint32_t numerator, uint32_t denominator,
		uint32_t maxWidth, uint32_t maxHeight);
// All images and mapping rectangles use top-left image coordinates.
// Cancellation returns an empty image without retaining a partially rebuilt sheet.
Image rebuild(const Sheet& sheet, const Image& original,
		const std::vector<std::optional<Image>>& tiles, uint32_t maxWidth, uint32_t maxHeight,
		const std::function<bool()>& cancelled = {});
}
