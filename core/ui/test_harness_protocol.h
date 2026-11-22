// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "deps/json/json.hpp"
#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace test_harness {

using Json = nlohmann::json;

inline void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

inline bool isIdentifier(const std::string& value)
{
	return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

inline void fields(const Json& value, std::initializer_list<const char *> allowed)
{
	require(value.is_object(), "Expected an object");
	for (auto it = value.begin(); it != value.end(); ++it)
	{
		bool known = false;
		for (const char *name : allowed)
			known = known || it.key() == name;
		require(known, "Unknown request field");
	}
}

inline std::int64_t integer(const Json& value, std::int64_t minimum, std::int64_t maximum)
{
	require(value.is_number_integer(), "Expected an integer");
	if (value.is_number_unsigned())
		require(value.get<std::uint64_t>() <= static_cast<std::uint64_t>(maximum), "Integer outside supported range");
	const auto result = value.get<std::int64_t>();
	require(result >= minimum && result <= maximum, "Integer outside supported range");
	return result;
}

inline Json parse(const std::string& bytes)
{
	require(!bytes.empty() && bytes.size() <= 65536, "Request exceeds 64 KiB");
	// Reject duplicate keys instead of silently selecting one conflicting value.
	std::vector<std::set<std::string>> keys;
	return Json::parse(bytes, [&keys](int depth, Json::parse_event_t event, Json& item) {
		require(depth <= 16, "Request is too deeply nested");
		if (event == Json::parse_event_t::object_start)
			keys.emplace_back();
		else if (event == Json::parse_event_t::object_end)
			keys.pop_back();
		else if (event == Json::parse_event_t::key)
			require(keys.back().insert(item.get<std::string>()).second, "Duplicate JSON key");
		return true;
	});
}

inline void validateEnvelope(const Json& request, const std::string& session, std::int64_t nowMs)
{
	fields(request, {"version", "session", "id", "expires_ms", "op", "args"});
	require(integer(request.at("version"), 1, 1) == 1, "Unsupported protocol version");
	require(request.at("session") == session, "Session mismatch");
	require(isIdentifier(request.at("id").get<std::string>()), "Invalid request id");
	const auto expires = integer(request.at("expires_ms"), 0, INT64_MAX);
	require(expires > nowMs && expires - nowMs <= 30000, "Request expired or deadline exceeds 30 seconds");
	require(request.at("op").is_string(), "Operation must be a string");
	require(request.at("args").is_object(), "Arguments must be an object");
}

inline void validateUiAction(const Json& args)
{
	require(isIdentifier(args.at("observation_id").get<std::string>()), "Invalid UI observation id");
	const auto action = args.at("action").get<std::string>();
	if (action == "click")
	{
		fields(args, {"observation_id", "action", "item_id", "x", "y"});
		if (args.contains("item_id"))
		{
			integer(args.at("item_id"), 1, UINT32_MAX);
			require(!args.contains("x") && !args.contains("y"), "Choose an item or coordinates");
		}
		else
		{
			integer(args.at("x"), 0, 8191);
			integer(args.at("y"), 0, 8191);
		}
	}
	else if (action == "key")
	{
		fields(args, {"observation_id", "action", "key"});
		const auto key = args.at("key").get<std::string>();
		require(!key.empty() && key.size() <= 64, "Invalid UI key chord");
	}
	else if (action == "text")
	{
		fields(args, {"observation_id", "action", "text"});
		const auto text = args.at("text").get<std::string>();
		require(!text.empty() && text.size() <= 4096 && text.find('\0') == std::string::npos, "Invalid UI text");
	}
	else if (action == "scroll")
	{
		fields(args, {"observation_id", "action", "x", "y", "delta_x", "delta_y"});
		integer(args.at("x"), 0, 8191);
		integer(args.at("y"), 0, 8191);
		integer(args.at("delta_x"), -100, 100);
		integer(args.at("delta_y"), -100, 100);
	}
	else if (action == "drag")
	{
		fields(args, {"observation_id", "action", "from_x", "from_y", "to_x", "to_y", "duration_ms"});
		for (const char *key : {"from_x", "from_y", "to_x", "to_y"})
			integer(args.at(key), 0, 8191);
		integer(args.at("duration_ms"), 80, 5000);
	}
	else
		throw std::runtime_error("Unknown native UI action");
}

// These masks are the public Dreamcast digital button bits, active high here.
// The device adapter performs the ordinary active-low Maple conversion.
inline constexpr std::array<const char *, 18> ButtonNames = {
	"C", "B", "A", "START", "UP", "DOWN", "LEFT", "RIGHT", "Z", "Y", "X", "D",
	"UP2", "DOWN2", "LEFT2", "RIGHT2", "RELOAD", "INSERT_CARD"
};

struct ControllerState {
	int port = 0;
	std::uint32_t buttons = 0;
	std::array<int, 4> axes{};
	std::array<int, 2> triggers{};
	int leaseMs = 500;

	static ControllerState fromJson(const Json& args)
	{
		fields(args, {"port", "buttons", "axes", "triggers", "lease_ms"});
		ControllerState result;
		result.port = static_cast<int>(integer(args.at("port"), 0, 3));
		result.leaseMs = static_cast<int>(integer(args.at("lease_ms"), 1, 2000));
		const auto& buttons = args.at("buttons");
		require(buttons.is_array() && buttons.size() <= ButtonNames.size(), "Invalid button list");
		for (const auto& button : buttons)
		{
			require(button.is_string(), "Button names must be strings");
			bool found = false;
			for (std::size_t i = 0; i < ButtonNames.size(); ++i)
				if (button == ButtonNames[i])
				{
					require((result.buttons & (1u << i)) == 0, "Duplicate button");
					result.buttons |= 1u << i;
					found = true;
				}
			require(found, "Unknown controller button");
		}
		const auto& axes = args.at("axes");
		const auto& triggers = args.at("triggers");
		require(axes.is_array() && axes.size() == 4, "Expected four stick axes");
		require(triggers.is_array() && triggers.size() == 2, "Expected two triggers");
		for (std::size_t i = 0; i < result.axes.size(); ++i)
			result.axes[i] = static_cast<int>(integer(axes[i], -32767, 32767));
		for (std::size_t i = 0; i < result.triggers.size(); ++i)
			result.triggers[i] = static_cast<int>(integer(triggers[i], 0, 65535));
		return result;
	}
};

} // namespace test_harness
