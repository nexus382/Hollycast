// Portions Copyright 2026 The Hollycast Authors
#include "TextureAtlas.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace texture_atlas
{
namespace
{
constexpr size_t MaxSheets = 4096;
constexpr size_t MaxRegions = 4096;
constexpr uint64_t MaxImageBytes = 512ull * 1024 * 1024;

bool overlaps(const Region& a, const Region& b)
{
	return a.x < b.x + b.width && b.x < a.x + a.width
			&& a.y < b.y + b.height && b.y < a.y + a.height;
}

bool validRegion(const Sheet& sheet, const Region& region)
{
	return region.width != 0 && region.height != 0 && region.x < sheet.width
			&& region.y < sheet.height && region.width <= sheet.width - region.x
			&& region.height <= sheet.height - region.y;
}

uint32_t unsignedValue(const nlohmann::json& value)
{
	if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX)
		throw std::runtime_error("atlas mapping requires unsigned 32-bit integers");
	return value.get<uint32_t>();
}

void validateImage(const Image& image)
{
	if (image.width > 16384 || image.height > 16384)
		throw std::runtime_error("atlas image exceeds supported dimensions");
	const uint64_t bytes = uint64_t(image.width) * image.height * 4;
	if (image.width == 0 || image.height == 0 || bytes > MaxImageBytes
			|| bytes != image.pixels.size())
		throw std::runtime_error("invalid atlas image dimensions or pixel data");
}
}

bool Region::operator==(const Region& other) const
{
	return x == other.x && y == other.y && width == other.width && height == other.height;
}

std::optional<Region> RegionDetector::submitTriangle(const std::array<std::array<float, 2>, 3>& uv)
{
	if (width == 0 || height == 0 || width > 8192 || height > 8192)
		return std::nullopt;
	std::array<std::array<uint32_t, 2>, 3> points;
	uint32_t minU = width * 2, minV = height * 2, maxU = 0, maxV = 0;
	for (size_t i = 0; i < uv.size(); i++)
	{
		const auto& point = uv[i];
		if (!std::isfinite(point[0]) || !std::isfinite(point[1])
				|| point[0] < 0 || point[0] > 1 || point[1] < 0 || point[1] > 1)
			return std::nullopt; // Wrapped/mirrored UV ranges do not identify a single crop.
		for (size_t axis = 0; axis < 2; axis++)
		{
			const double coordinate = double(point[axis]) * (axis == 0 ? width : height) * 2;
			const double rounded = std::round(coordinate);
			// Chronicles submits individually clipped triangles. Arbitrary clipping intersections
			// must not become crop boundaries; authored edges normally lie on texels or texel centers.
			if (std::abs(coordinate - rounded) > 0.002)
				return std::nullopt;
			points[i][axis] = static_cast<uint32_t>(rounded);
		}
		minU = std::min(minU, points[i][0]);
		maxU = std::max(maxU, points[i][0]);
		minV = std::min(minV, points[i][1]);
		maxV = std::max(maxV, points[i][1]);
	}
	if (maxU - minU < 2 || maxV - minV < 2)
		return std::nullopt;
	unsigned corners = 0;
	for (const auto& point : points)
	{
		if ((point[0] != minU && point[0] != maxU) || (point[1] != minV && point[1] != maxV))
			return std::nullopt;
		corners |= 1u << (unsigned(point[0] == maxU) + 2 * unsigned(point[1] == maxV));
	}
	if (corners != 7 && corners != 11 && corners != 13 && corners != 14)
		return std::nullopt;
	const uint32_t left = minU / 2, top = minV / 2;
	const uint32_t right = (maxU + 1) / 2, bottom = (maxV + 1) / 2;
	// Inset UVs on a standalone image (e.g. the Dreamcast spiral) are still whole-image use.
	if (right - left + 2 >= width && bottom - top + 2 >= height)
		return std::nullopt;
	const std::array<uint32_t, 4> bounds { minU, minV, maxU, maxV };
	auto found = triangles.find(bounds);
	if (found == triangles.end())
	{
		if (triangles.size() >= MaxRegions)
			return std::nullopt;
		found = triangles.emplace(bounds, 0).first;
	}
	uint16_t& observed = found->second;
	constexpr uint16_t Confirmed = 1;
	if ((observed & Confirmed) != 0)
		return std::nullopt;
	observed |= uint16_t(1u << corners);
	// The halves may belong to different strips or frames. Only opposite missing corners
	// cover the rectangle: sharing a side would leave a gap and double-cover another area.
	if (!((observed & (1u << 7)) && (observed & (1u << 14)))
			&& !((observed & (1u << 11)) && (observed & (1u << 13))))
		return std::nullopt;
	observed |= Confirmed;
	// Dumped PNGs are flipped vertically relative to PVR sampling coordinates.
	return Region { left, height - bottom, right - left, bottom - top };
}

bool addRegion(Sheet& sheet, const Region& region)
{
	if (!validRegion(sheet, region))
		return false;
	auto contained = sheet.regions.end();
	for (auto existing = sheet.regions.begin(); existing != sheet.regions.end(); ++existing)
	{
		if (!overlaps(*existing, region))
			continue;
		if (*existing == region || existing->x < region.x || existing->y < region.y
				|| existing->x + existing->width > region.x + region.width
				|| existing->y + existing->height > region.y + region.height || contained != sheet.regions.end())
			return false; // Never combine independently identified pieces or partially overlapping regions.
		contained = existing;
	}
	if (contained != sheet.regions.end())
	{
		// A complete face discovered later supersedes its clipped partial rectangle.
		*contained = region;
		return true;
	}
	if (sheet.regions.size() >= MaxRegions)
		return false;
	sheet.regions.push_back(region);
	return true;
}

std::string imageFilename(uint32_t hash)
{
	std::ostringstream name;
	name.imbue(std::locale::classic());
	name << std::hex << hash << ".png";
	return name.str();
}

std::string regionFilename(uint32_t hash, const Region& region)
{
	std::ostringstream name;
	name.imbue(std::locale::classic());
	name << "atlas_" << std::hex << hash << std::dec << '_' << region.x << '_' << region.y
			<< '_' << region.width << '_' << region.height << ".png";
	return name.str();
}

std::vector<Sheet> parseMapping(const std::string& text)
{
	if (text.size() > 4 * 1024 * 1024)
		throw std::runtime_error("atlas mapping exceeds 4 MiB");
	const auto root = nlohmann::json::parse(text);
	if (root.at("version") != 1 || !root.at("sheets").is_array() || root.at("sheets").size() > MaxSheets)
		throw std::runtime_error("unsupported atlas mapping version or sheet count");
	std::vector<Sheet> sheets;
	std::set<uint32_t> hashes;
	for (const auto& entry : root.at("sheets"))
	{
		Sheet sheet { unsignedValue(entry.at("hash")), unsignedValue(entry.at("width")),
				unsignedValue(entry.at("height")), {} };
		if (!hashes.insert(sheet.hash).second || sheet.width == 0 || sheet.height == 0
				|| sheet.width > 8192 || sheet.height > 8192 || !entry.at("regions").is_array()
				|| entry.at("regions").empty() || entry.at("regions").size() > MaxRegions)
			throw std::runtime_error("invalid atlas sheet dimensions, hash or region count");
		for (const auto& box : entry.at("regions"))
		{
			if (!box.is_array() || box.size() != 4)
				throw std::runtime_error("atlas region must contain x, y, width and height");
			Region region { unsignedValue(box[0]), unsignedValue(box[1]),
					unsignedValue(box[2]), unsignedValue(box[3]) };
			const size_t previousCount = sheet.regions.size();
			if (!addRegion(sheet, region) || sheet.regions.size() != previousCount + 1)
				throw std::runtime_error("atlas regions overlap or extend outside their sheet");
		}
		sheets.push_back(std::move(sheet));
	}
	return sheets;
}

std::string writeMapping(const std::vector<Sheet>& sheets)
{
	nlohmann::json entries = nlohmann::json::array();
	for (const Sheet& sheet : sheets)
	{
		nlohmann::json regions = nlohmann::json::array();
		for (const Region& region : sheet.regions)
			regions.push_back({ region.x, region.y, region.width, region.height });
		entries.push_back({ { "hash", sheet.hash }, { "width", sheet.width },
				{ "height", sheet.height }, { "regions", std::move(regions) } });
	}
	return nlohmann::json({ { "version", 1 }, { "sheets", std::move(entries) } }).dump(2) + '\n';
}

Image rebuild(const Sheet& sheet, const Image& original,
		const std::vector<std::optional<Image>>& tiles, uint32_t maxWidth, uint32_t maxHeight)
{
	validateImage(original);
	if (original.width != sheet.width || original.height != sheet.height || tiles.size() != sheet.regions.size())
		throw std::runtime_error("atlas original or tile count does not match the mapping");
	uint32_t numerator = 0, denominator = 1;
	for (size_t i = 0; i < tiles.size(); i++)
	{
		if (!validRegion(sheet, sheet.regions[i]))
			throw std::runtime_error("invalid atlas region");
		if (!tiles[i])
			continue;
		validateImage(*tiles[i]);
		const Region& region = sheet.regions[i];
		const uint32_t divisor = std::gcd(tiles[i]->width, region.width);
		const uint32_t tileNumerator = tiles[i]->width / divisor, tileDenominator = region.width / divisor;
		if (uint64_t(region.height) * tileNumerator != uint64_t(tiles[i]->height) * tileDenominator
				|| (numerator != 0 && (numerator != tileNumerator || denominator != tileDenominator)))
			throw std::runtime_error("atlas piece has a different scale: " + regionFilename(sheet.hash, region));
		numerator = tileNumerator;
		denominator = tileDenominator;
	}
	if (numerator == 0)
		numerator = 1;
	const auto scaled = [numerator, denominator](uint32_t value) -> uint64_t {
		// A shared fractional scale can put sheet boundaries between output pixels.
		// Round consistently so adjacent regions retain a shared boundary.
		return (uint64_t(value) * numerator + denominator / 2) / denominator;
	};
	const uint64_t width = scaled(sheet.width), height = scaled(sheet.height);
	if (width == 0 || height == 0 || width > maxWidth || height > maxHeight || width > 16384 || height > 16384
			|| width * height * 4 > MaxImageBytes)
		throw std::runtime_error("rebuilt atlas exceeds renderer or memory limits");
	for (size_t i = 0; i < tiles.size(); i++)
		if (tiles[i])
		{
			const Region& region = sheet.regions[i];
			if (scaled(region.x) + tiles[i]->width > width || scaled(region.y) + tiles[i]->height > height)
				throw std::runtime_error("scaled atlas piece extends outside its sheet");
		}
	Image output { static_cast<uint32_t>(width), static_cast<uint32_t>(height), {} };
	output.pixels.resize(static_cast<size_t>(width * height * 4));
	// Keep unknown/missing regions intact, scaling their original pixels to the same size.
	for (uint32_t y = 0; y < output.height; y++)
		for (uint32_t x = 0; x < output.width; x++)
			std::copy_n(&original.pixels[(size_t(uint64_t(y) * denominator / numerator) * sheet.width
					+ uint64_t(x) * denominator / numerator) * 4], 4,
					&output.pixels[(size_t(y) * output.width + x) * 4]);
	for (size_t i = 0; i < tiles.size(); i++)
	{
		if (!tiles[i])
			continue;
		const Region& region = sheet.regions[i];
		const Image& tile = *tiles[i];
		const size_t left = static_cast<size_t>(scaled(region.x)), top = static_cast<size_t>(scaled(region.y));
		for (uint32_t y = 0; y < tile.height; y++)
			std::copy_n(&tile.pixels[size_t(y) * tile.width * 4], size_t(tile.width) * 4,
					&output.pixels[((top + y) * output.width + left) * 4]);
	}
	return output;
}
}
