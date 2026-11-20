// Portions Copyright 2026 The Hollycast Authors
#include "ui/test_harness_protocol.h"
#include "ui/test_harness.h"
#include "ui/test_harness_watch.h"
#include <functional>
#include <iostream>

using namespace test_harness;

static int checks;

static void expect(bool condition)
{
	++checks;
	if (!condition)
		throw std::runtime_error("Unexpected protocol test result");
}

static void reject(const std::function<void()>& action)
{
	bool rejected = false;
	try { action(); }
	catch (const std::exception&) { rejected = true; }
	expect(rejected);
}

int main()
{
	try {
		const char *existingRoot = std::getenv("HOLLYCAST_HARNESS_DIR");
		const std::string originalRoot = existingRoot ? existingRoot : "";
#ifdef _WIN32
		_putenv_s("HOLLYCAST_HARNESS_DIR", "ordinary-build-must-reject");
#else
		setenv("HOLLYCAST_HARNESS_DIR", "ordinary-build-must-reject", 1);
#endif
		reject([] { prepareStorage(); });
#ifdef _WIN32
		_putenv_s("HOLLYCAST_HARNESS_DIR", originalRoot.c_str());
#else
		if (existingRoot)
			setenv("HOLLYCAST_HARNESS_DIR", originalRoot.c_str(), 1);
		else
			unsetenv("HOLLYCAST_HARNESS_DIR");
#endif
		const std::string session(32, 'a');
		Json request = {{"version", 1}, {"session", session}, {"id", std::string(32, 'b')},
			{"expires_ms", 1100}, {"op", "observe"}, {"args", Json::object()}};
		validateEnvelope(parse(request.dump()), session, 1000);
		expect(isIdentifier(session));
		expect(!isIdentifier("../response"));
		for (const auto& replacement : {Json(0), Json(2), Json(true), Json(1.0)})
		{
			auto invalid = request;
			invalid["version"] = replacement;
			reject([&] { validateEnvelope(invalid, session, 1000); });
		}
		for (const auto deadline : {999, 1000, 31001})
		{
			auto invalid = request;
			invalid["expires_ms"] = deadline;
			reject([&] { validateEnvelope(invalid, session, 1000); });
		}
		reject([&] { validateEnvelope(request, std::string(32, 'c'), 1000); });
		auto invalid = request;
		invalid["id"] = "../../escape";
		reject([&] { validateEnvelope(invalid, session, 1000); });
		invalid = request;
		invalid["unexpected"] = true;
		reject([&] { validateEnvelope(invalid, session, 1000); });
		reject([] { parse("{\"id\":1,\"id\":2}"); });
		reject([] { parse("{\"args\":{\"port\":0,\"port\":1}}"); });
		reject([] { parse(std::string(65537, ' ')); });
		reject([] { parse("{\"id\":"); });
		reject([] { parse(std::string(18, '[') + "0" + std::string(18, ']')); });
		reject([] { integer(Json(std::uint64_t(-1)), -32767, 32767); });
		reject([] { integer(Json(-1), 0, 3); });
		reject([] { integer(Json(true), 0, 3); });
		Json input = {{"port", 3}, {"buttons", {"A", "START", "INSERT_CARD"}},
			{"axes", {-32767, 32767, 0, 0}}, {"triggers", {0, 65535}}, {"lease_ms", 2000}};
		const auto controller = ControllerState::fromJson(input);
		expect(controller.port == 3);
		expect(controller.buttons == ((1u << 2) | (1u << 3) | (1u << 17)));
		expect(controller.axes[0] == -32767 && controller.axes[1] == 32767);
		expect(controller.triggers[0] == 0 && controller.triggers[1] == 65535);
		for (const auto port : {-1, 4})
		{
			auto bad = input;
			bad["port"] = port;
			reject([&] { ControllerState::fromJson(bad); });
		}
		for (const auto lease : {0, 2001})
		{
			auto bad = input;
			bad["lease_ms"] = lease;
			reject([&] { ControllerState::fromJson(bad); });
		}
		for (const auto& pair : {std::pair<const char *, Json>{"buttons", {"A", "A"}},
			{"buttons", {"FAKE"}}, {"buttons", {1}}, {"axes", {0, 0}},
			{"axes", {32768, 0, 0, 0}}, {"axes", {0.5, 0, 0, 0}},
			{"triggers", {-1, 0}}, {"triggers", {0, 65536}}})
		{
			auto bad = input;
			bad[pair.first] = pair.second;
			reject([&] { ControllerState::fromJson(bad); });
		}
		expect(parse("{\"a\":{\"x\":1},\"b\":{\"x\":2}}").at("b").at("x") == 2);
		const auto observed = std::string(32, 'e');
		Json click = {{"observation_id", observed}, {"action", "click"}, {"item_id", 12}};
		validateUiAction(click);
		auto detailedClick = click;
		detailedClick["window"] = "Settings##NewSettings";
		detailedClick["modifiers"] = {"Ctrl", "Shift"};
		detailedClick["mouse_button"] = "middle";
		detailedClick["click_count"] = 2;
		validateUiAction(detailedClick);
		detailedClick["window"] = std::string(512, 'W');
		validateUiAction(detailedClick);
		auto badClick = click;
		badClick["x"] = 0;
		reject([&] { validateUiAction(badClick); });
		badClick = click;
		badClick["window"] = "Settings\n";
		reject([&] { validateUiAction(badClick); });
		badClick = click;
		badClick["window"] = std::string(513, 'W');
		reject([&] { validateUiAction(badClick); });
		badClick = click;
		badClick["modifiers"] = {"Ctrl", "Ctrl"};
		reject([&] { validateUiAction(badClick); });
		badClick = click;
		badClick["modifiers"] = {"Meta"};
		reject([&] { validateUiAction(badClick); });
		badClick = click;
		badClick["mouse_button"] = "x1";
		reject([&] { validateUiAction(badClick); });
		for (const int count : {0, 3})
		{
			badClick = click;
			badClick["click_count"] = count;
			reject([&] { validateUiAction(badClick); });
		}
		reject([&] { validateUiAction({{"observation_id", observed}, {"action", "click"},
			{"x", 1}, {"y", 1}, {"window", "Settings"}}); });
		reject([&] { validateUiAction({{"observation_id", observed}, {"action", "click"}, {"x", -1}, {"y", 0}}); });
		reject([&] { validateUiAction({{"observation_id", observed}, {"action", "key"}, {"key", ""}}); });
		reject([&] { validateUiAction({{"observation_id", observed}, {"action", "text"}, {"text", std::string(4097, 'x')}}); });
		Json watch = {{"name", "test"}, {"region", "master_ram"}, {"offset", 16}, {"type", "s16"}};
		expect(Watch::fromJson(watch).decode(65535).at("value") == -1);
		watch["type"] = "f32";
		expect(Watch::fromJson(watch).decode(0x3f800000).at("value") == 1);
		expect(!Watch::fromJson(watch).decode(0x7fc00000).at("valid").get<bool>());
		watch["type"] = "s32";
		expect(Watch::fromJson(watch).decode(UINT32_MAX).at("value") == -1);
		auto invalidWatch = watch;
		invalidWatch["region"] = "mmio";
		reject([&] { Watch::fromJson(invalidWatch); });
		invalidWatch = watch;
		invalidWatch["offset"] = -1;
		reject([&] { Watch::fromJson(invalidWatch); });
		invalidWatch = watch;
		invalidWatch["type"] = "guess";
		reject([&] { Watch::fromJson(invalidWatch); });
		Json dreamcastWatch = {{"name", "main_ram_value"}, {"region", "main_ram"}, {"offset", 16}, {"type", "s16"}};
		Json profile = {{"version", 1}, {"id", "system-smoke"}, {"platform", "dreamcast"}, {"game_id", "MAXIMUM SPEED"},
			{"content_sha256", std::string(64, 'f')}};
		profile["watches"] = Json::array({dreamcastWatch});
		for (const auto *platform : {"dreamcast", "naomi", "naomi2", "atomiswave", "systemsp"})
		{
			profile["platform"] = platform;
			expect(WatchProfile::fromJson(profile).watches.size() == 1);
		}
		profile["platform"] = "unknown";
		reject([&] { WatchProfile::fromJson(profile); });
		profile["platform"] = "naomi";
		profile["watches"] = Json::array({dreamcastWatch, watch});
		reject([&] { WatchProfile::fromJson(profile); });
		profile["platform"] = "hikaru";
		profile["rom_crc32"] = {1, 2, 3};
		profile["watches"] = Json::array({watch});
		expect(WatchProfile::fromJson(profile).watches.size() == 1);
		profile["watches"].push_back(watch);
		reject([&] { WatchProfile::fromJson(profile); });
		profile["watches"] = Json::array({dreamcastWatch, watch});
		reject([&] { WatchProfile::fromJson(profile); });
		std::cout << checks << " protocol checks passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "Protocol check failed after " << checks << " checks: " << error.what() << '\n';
		return 1;
	}
}
