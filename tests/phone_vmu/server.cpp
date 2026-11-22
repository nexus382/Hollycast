// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "network/phone_vmu.h"
#include "oslib/http_server.h"
#include <asio.hpp>
#include <array>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace
{
void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

struct Address
{
	unsigned short port;
	std::string path;
};

Address parse(const std::string& url)
{
	const auto colon = url.find(':', 7);
	const auto slash = url.find('/', colon);
	return { static_cast<unsigned short>(std::stoi(url.substr(colon + 1, slash - colon - 1))), url.substr(slash) };
}

std::string request(Address address, const std::string& path, const std::string& method = "GET",
		const std::string& data = "x", const std::string& headers = "", std::size_t declaredLength = std::string::npos)
{
	asio::io_context io;
	asio::ip::tcp::socket socket(io);
	socket.connect({asio::ip::address_v4::loopback(), address.port});
	const auto text = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
		+ headers + (method == "POST" ? "Content-Length: " + std::to_string(declaredLength == std::string::npos ? data.size() : declaredLength) + "\r\n\r\n" + data : "\r\n");
	asio::write(socket, asio::buffer(text));
	std::string response;
	std::array<char, 4096> buffer;
	asio::error_code ec;
	for (;;)
	{
		const auto count = socket.read_some(asio::buffer(buffer), ec);
		response.append(buffer.data(), count);
		// Read a complete HTTP response without waiting for EOF. Rejecting a
		// declared oversized body may reset a socket with unread request bytes.
		const auto split = response.find("\r\n\r\n");
		const auto length = response.find("\r\nContent-Length: ");
		if (split != std::string::npos && length != std::string::npos
				&& response.size() >= split + 4 + std::stoul(response.substr(length + 18)))
			return response;
		if (ec)
		{
			check(ec == asio::error::eof, ("HTTP read failed for " + path + ": " + ec.message()).c_str());
			return response;
		}
	}
}

std::string body(const std::string& response)
{
	const auto split = response.find("\r\n\r\n");
	check(split != std::string::npos, "Missing HTTP headers");
	return response.substr(split + 4);
}

std::string header(const std::string& response, const std::string& name)
{
	const auto start = response.find("\r\n" + name + ": ");
	check(start != std::string::npos, "Missing response header");
	const auto value = start + name.size() + 4;
	return response.substr(value, response.find("\r\n", value) - value);
}
}

#include "maple_sync.inc"

int main(int argc, char **argv)
{
	try
	{
		check(!phone_vmu::status().running, "Sharing must start disabled");
		RequestParser defaultParser;
		Request parserRequest;
		const std::string largeHeader = "POST / HTTP/1.1\r\nContent-Length: 5000\r\n\r\n";
		check(std::get<0>(defaultParser.parse(parserRequest, largeHeader.begin(), largeHeader.end())) == RequestParser::bad,
			"Phone VMU body limits must not increase the shared HTTP parser's default limit");
		std::array<std::uint32_t, 48 * 32> pixels {};
		pixels[0] = pixels[7] = pixels[8] = pixels.back() = 0xff000000;
		phone_vmu::publishScreen(0, pixels.data(), true);
		phone_vmu::publishScreen(7, pixels.data(), true);
		phone_vmu::publishScreen(-1, pixels.data(), true);
		phone_vmu::publishScreen(8, pixels.data(), true);
		phone_vmu::start();
		auto status = phone_vmu::status();
		check(status.error.empty(), status.error.c_str());
		check(status.running && !status.urls.empty() && !status.connected, "Server did not start ready for pairing");
		// The first pairing link must follow the active route, rather than a
		// numerically earlier virtual interface such as Windows' WSL adapter.
		asio::io_context routeIo;
		asio::ip::udp::socket routeSocket(routeIo);
		asio::error_code routeError;
		routeSocket.open(asio::ip::udp::v4(), routeError);
		if (!routeError)
			routeSocket.connect({ asio::ip::make_address_v4("1.1.1.1"), 9 }, routeError);
		if (!routeError)
		{
			const auto endpoint = routeSocket.local_endpoint(routeError);
			if (!routeError)
			{
				const auto prefix = "http://" + endpoint.address().to_string() + ":";
				const bool published = std::any_of(status.urls.begin(), status.urls.end(), [&](const auto& url) { return url.find(prefix) == 0; });
				if (published)
					check(status.urls.front().find(prefix) == 0, "Pairing selected a virtual interface ahead of the active network");
			}
		}
		const Address address = parse(status.urls.front());
		if (argc >= 2 && std::string(argv[1]) == "--preview")
		{
			// Synthetic LCD for manual/browser acceptance checks; no game files needed.
			for (int y = 0; y < 32; ++y)
				for (int x = 0; x < 48; ++x)
					pixels[y * 48 + x] = (x == 0 || x == 47 || y == 0 || y == 31 || ((x / 4 + y / 4) % 2 == 0)) ? 0xff000000 : 0xffffffff;
			phone_vmu::publishScreen(0, pixels.data(), true);
			if (argc == 3)
			{
				std::ifstream input(argv[2], std::ios::binary);
				const std::vector<std::uint8_t> card((std::istreambuf_iterator<char>(input)), {});
				check(card.size() == 131072, "Preview card must be a 128 KiB image");
				phone_vmu::publishCard(0, card.data(), card.size());
			}
			std::cout << status.urls.front() << std::endl;
			std::cin.get();
			phone_vmu::stop();
			return 0;
		}
		const std::string html = request(address, address.path);
		check(html.find("HTTP/1.1 200") == 0 && body(html).find("<canvas") != std::string::npos, "Pairing page unavailable");
		check(html.find("Cache-Control: no-store") != std::string::npos && html.find("frame-ancestors 'none'") != std::string::npos,
			"Pairing page missing privacy headers");
		check(body(request(address, address.path + "vmu-core.js")).find("const VMS") != std::string::npos, "VMU CPU was not served");
		check(body(request(address, address.path + "vmu-core.LICENSE")).find("GNU GENERAL PUBLIC LICENSE") != std::string::npos, "VMU license was not served");
		const auto wakeMedia = body(request(address, address.path + "wake-media.js"));
		check(wakeMedia.find("const PhoneVmuWakeMedia") != std::string::npos && wakeMedia.find("Copyright (c) Rich Tibbett") != std::string::npos,
			"Offline keep-awake media or its license was not served");
		check(html.find("media-src data:") != std::string::npos, "CSP blocked the local keep-awake video");
		std::vector<std::uint8_t> card(131072, 0x55);
		phone_vmu::publishCard(0, card.data(), card.size());
		phone_vmu::updateCard(0, 100, reinterpret_cast<const std::uint8_t *>("abc"), 3);
		phone_vmu::updateCard(0, 131072, card.data(), 1);
		phone_vmu::updateCard(-1, 0, card.data(), 1);
		phone_vmu::publishCard(0, card.data(), 10);
		const auto copy = body(request(address, address.path + "card/0"));
		check(copy.size() == 131072 && copy.substr(100, 3) == "abc" && copy.back() == 0x55, "Card snapshot or bounded updates failed");
		check(request(address, address.path + "card/1").find("HTTP/1.1 404") == 0, "Empty slot exposed a card");
		const auto frame = body(request(address, address.path + "frame/0"));
		check(frame.size() == 193 && frame[0] == 1, "Active frame has wrong status or size");
		std::string expectedFrame(193, '\0');
		expectedFrame[0] = 1;
		expectedFrame[6] = static_cast<char>(0x80); // Producer's last pixel is the browser's top-right pixel.
		expectedFrame[187] = static_cast<char>(0x81); // Producer's first row becomes the bottom row, without mirroring X.
		expectedFrame[188] = 1;
		check(frame == expectedFrame, "LCD rows must be top-down with LSB-first pixels and unchanged horizontal order");
		check(phone_vmu::status().connected, "Phone activity was not reported");
		const auto inactive = body(request(address, address.path + "frame/1"));
		check(inactive == std::string(193, '\0'), "Inactive slot leaked another slot's pixels");
		check(body(request(address, address.path + "frame/7")) == frame, "Last VMU slot unavailable");
		phone_vmu::publishScreen(0, nullptr, false);
		check(body(request(address, address.path + "frame/0")) == inactive, "Disconnect retained stale pixels");
		check(body(request(address, address.path + "frame/7")) == frame, "Slot reset changed another phone's display");
		pixels.fill(0);
		phone_vmu::publishScreen(7, pixels.data(), true);
		check(body(request(address, address.path + "frame/7")) == std::string(1, '\1') + std::string(192, '\0'),
			"Blanking did not reach an active VMU");
		for (const std::string& path : std::vector<std::string>{ "/", "/wrong/frame/0", address.path + "frame/8", address.path + "frame/-1",
				address.path + "frame/0/extra", address.path + "frame/0?ignored", address.path + "extra" })
		{
			const auto response = request(address, path);
			check(response.find("HTTP/1.1 404") == 0, ("Invalid or unpaired path was accepted: " + path + " => " + response.substr(0, 40)).c_str());
		}
		check(request(address, address.path, "POST").find("HTTP/1.1 405") == 0, "Write request was accepted");
		check(request(address, address.path + "%2e%2e/").find("HTTP/1.1 400") == 0, "Traversal was accepted");
		testMapleSync(address);

		// Slow clients and ongoing producers must not prevent immediate shutdown.
		asio::io_context io;
		asio::ip::tcp::socket slowClient(io);
		slowClient.connect({asio::ip::address_v4::loopback(), address.port});
		asio::write(slowClient, asio::buffer("GET /", 5));
		const auto beforeStop = std::chrono::steady_clock::now();
		phone_vmu::stop();
		check(std::chrono::steady_clock::now() - beforeStop < std::chrono::seconds(1), "Shutdown waited for a slow client");
		check(!phone_vmu::status().running && phone_vmu::status().urls.empty(), "Sharing did not stop");
		asio::ip::tcp::socket probe(io);
		asio::error_code ec;
		probe.connect({asio::ip::address_v4::loopback(), address.port}, ec);
		check(static_cast<bool>(ec), "Listener remained open after disabling sharing");
		phone_vmu::start();
		status = phone_vmu::status();
		check(status.running, "Sharing did not restart");
		const auto restarted = parse(status.urls.front());
		check(restarted.path != address.path, "Restart reused the old pairing token");
		check(request(restarted, address.path).find("HTTP/1.1 404") == 0, "Old pairing link remained authorized");
		check(body(request(restarted, restarted.path + "frame/7"))[0] == 1, "Pairing mid-game lost the latest LCD");
		phone_vmu::stop();
		phone_vmu::stop();
		std::cout << "Phone VMU server checks passed\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		phone_vmu::stop();
		std::cerr << e.what() << '\n';
		return 1;
	}
}
