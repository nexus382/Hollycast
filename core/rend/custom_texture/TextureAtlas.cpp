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

std::optional<Region> quadRegion(const std::array<std::array<float, 2>, 4>& uv,
		uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0 || width > 8192 || height > 8192)
		return std::nullopt;
	float minU = 1, minV = 1, maxU = 0, maxV = 0;
	for (const auto& point : uv)
	{
		if (!std::isfinite(point[0]) || !std::isfinite(point[1])
				|| point[0] < 0 || point[0] > 1 || point[1] < 0 || point[1] > 1)
			return std::nullopt; // Wrapped/mirrored UV ranges do not identify a single crop.
		minU = std::min(minU, point[0]); maxU = std::max(maxU, point[0]);
		minV = std::min(minV, point[1]); maxV = std::max(maxV, point[1]);
	}
	if (maxU - minU < 1.0f / width || maxV - minV < 1.0f / height)
		return std::nullopt;
	unsigned corners = 0;
	std::array<unsigned, 4> cornerIds {};
	for (size_t i = 0; i < uv.size(); i++)
	{
		const auto& point = uv[i];
		// Accept only paired triangles covering an axis-aligned UV rectangle.
		// Arbitrary UV islands stay in the original sheet instead of becoming misleading crops.
		const bool right = std::abs(point[0] - maxU) < 0.001f / width;
		const bool bottom = std::abs(point[1] - maxV) < 0.001f / height;
		if ((!right && std::abs(point[0] - minU) >= 0.001f / width)
				|| (!bottom && std::abs(point[1] - minV) >= 0.001f / height))
			return std::nullopt;
		cornerIds[i] = unsigned(right) + 2 * unsigned(bottom);
		corners |= 1u << cornerIds[i];
	}
	// Vertices 1 and 2 are shared by the two strip triangles; they must form the diagonal.
	if (corners != 15 || (cornerIds[1] ^ cornerIds[2]) != 3)
		return std::nullopt;
	const uint32_t left = static_cast<uint32_t>(std::floor(minU * width));
	const uint32_t top = static_cast<uint32_t>(std::floor(minV * height));
	const uint32_t right = static_cast<uint32_t>(std::ceil(maxU * width));
	const uint32_t bottom = static_cast<uint32_t>(std::ceil(maxV * height));
	if (right <= left || bottom <= top || (right - left == width && bottom - top == height))
		return std::nullopt;
	// Dumped PNGs are flipped vertically relative to PVR sampling coordinates.
	return Region { left, height - bottom, right - left, bottom - top };
}

bool addRegion(Sheet& sheet, const Region& region)
{
	if (!validRegion(sheet, region) || sheet.regions.size() >= MaxRegions)
		return false;
	for (const Region& existing : sheet.regions)
		if (overlaps(existing, region))
			return false; // Includes duplicates; never overwrite a previously exported piece.
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
			if (!addRegion(sheet, region))
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
		const uint64_t result = uint64_t(value) * numerator;
		if (result % denominator != 0)
			throw std::runtime_error("atlas scale places a sheet boundary between pixels");
		return result / denominator;
	};
	const uint64_t width = scaled(sheet.width), height = scaled(sheet.height);
	if (width > maxWidth || height > maxHeight || width > 16384 || height > 16384
			|| width * height * 4 > MaxImageBytes)
		throw std::runtime_error("rebuilt atlas exceeds renderer or memory limits");
	for (size_t i = 0; i < tiles.size(); i++)
		if (tiles[i])
		{
			scaled(sheet.regions[i].x);
			scaled(sheet.regions[i].y);
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
