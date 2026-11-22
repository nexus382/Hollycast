// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "test_harness_protocol.h"
#include <cmath>
#include <cstring>

namespace test_harness {

struct Watch {
	std::string name, region, type, unit;
	std::uint32_t offset = 0;
	unsigned size = 0;
	double scale = 1, bias = 0;
	Json labels = Json::object();

	static Watch fromJson(const Json& value)
	{
		fields(value, {"name", "region", "offset", "type", "scale", "bias", "unit", "labels"});
		Watch result;
		result.name = value.at("name").get<std::string>();
		require(!result.name.empty() && result.name.size() <= 64
			&& result.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos,
			"Invalid watch name");
		result.region = value.at("region").get<std::string>();
		require(result.region == "main_ram" || result.region == "master_ram" || result.region == "slave_ram"
			|| result.region == "command_ram" || result.region == "bram", "Unknown watch memory region");
		result.offset = static_cast<std::uint32_t>(integer(value.at("offset"), 0, 32 * 1024 * 1024 - 1));
		result.type = value.at("type").get<std::string>();
		if (result.type == "u8" || result.type == "s8") result.size = 1;
		else if (result.type == "u16" || result.type == "s16") result.size = 2;
		else if (result.type == "u32" || result.type == "s32" || result.type == "f32") result.size = 4;
		else throw std::runtime_error("Unknown watch type");
		result.scale = value.value("scale", 1.0);
		result.bias = value.value("bias", 0.0);
		require(std::isfinite(result.scale) && std::isfinite(result.bias) && std::abs(result.scale) <= 1e9
			&& std::abs(result.bias) <= 1e12, "Invalid watch scaling");
		result.unit = value.value("unit", std::string());
		require(result.unit.size() <= 64, "Watch unit is too long");
		if (value.contains("labels"))
		{
			result.labels = value.at("labels");
			require(result.labels.is_object() && result.labels.size() <= 64, "Invalid watch labels");
			for (const auto& label : result.labels.items())
				require(label.key().size() <= 16 && label.value().is_string() && label.value().get<std::string>().size() <= 128,
					"Invalid watch label");
		}
		return result;
	}

	Json decode(std::uint32_t raw) const
	{
		double value = raw;
		if (type == "s8") value = raw < 0x80 ? raw : double(raw) - 256;
		else if (type == "s16") value = raw < 0x8000 ? raw : double(raw) - 65536;
		else if (type == "s32") value = raw < 0x80000000u ? raw : double(raw) - 4294967296.0;
		else if (type == "f32")
		{
			float number;
			std::memcpy(&number, &raw, sizeof(number));
			value = number;
		}
		value = value * scale + bias;
		Json result = {{"raw", raw}, {"value", std::isfinite(value) ? Json(value) : Json()},
			{"valid", std::isfinite(value)}, {"type", type}, {"unit", unit}, {"region", region}, {"offset", offset}};
		const auto label = labels.find(std::to_string(raw));
		if (label != labels.end()) result["label"] = *label;
		return result;
	}
};

struct WatchProfile {
	std::string id, platform, gameId, contentHash;
	std::array<std::uint32_t, 3> romCrc32{};
	std::vector<Watch> watches;
	std::uint64_t generation = 0;

	static WatchProfile fromJson(const Json& value)
	{
		fields(value, {"version", "id", "platform", "game_id", "content_sha256", "rom_crc32", "watches"});
		integer(value.at("version"), 1, 1);
		WatchProfile result;
		result.id = value.at("id").get<std::string>();
		result.platform = value.at("platform").get<std::string>();
		result.gameId = value.at("game_id").get<std::string>();
		require(!result.id.empty() && result.id.size() <= 128 && !result.gameId.empty() && result.gameId.size() <= 128,
			"Invalid profile identity");
		require(result.platform == "hikaru" || result.platform == "dreamcast" || result.platform == "naomi"
			|| result.platform == "naomi2" || result.platform == "atomiswave" || result.platform == "systemsp",
			"Unknown watch platform");
		result.contentHash = value.at("content_sha256").get<std::string>();
		require(result.contentHash.size() == 64 && result.contentHash.find_first_not_of("0123456789abcdef") == std::string::npos,
			"Invalid profile content hash");
		if (result.platform == "hikaru")
		{
			const auto& crc = value.at("rom_crc32");
			require(crc.is_array() && crc.size() == 3, "Hikaru profiles require BIOS/program/data CRC32 identities");
			for (std::size_t i = 0; i < 3; ++i)
				result.romCrc32[i] = static_cast<std::uint32_t>(integer(crc[i], 0, UINT32_MAX));
		}
		const auto& watches = value.at("watches");
		require(watches.is_array() && !watches.empty() && watches.size() <= 64, "Expected 1..64 memory watches");
		std::set<std::string> names;
		for (const auto& value : watches)
		{
			auto watch = Watch::fromJson(value);
			require(names.insert(watch.name).second, "Duplicate watch name");
			require(result.platform == "hikaru" ? watch.region != "main_ram" : watch.region == "main_ram",
				"Watch region does not belong to the selected platform");
			result.watches.push_back(std::move(watch));
		}
		return result;
	}
};

} // namespace test_harness
