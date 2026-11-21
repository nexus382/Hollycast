// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "network/phone_vmu.h"
#include "oslib/http_server.h"
#include <asio.hpp>
#include <array>
#include <atomic>
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
		const std::string& data = "x", const std::string& headers = "", std::size_t declaredLength = std::string::npos,
		const std::string& host = "127.0.0.1")
{
	asio::io_context io;
	asio::ip::tcp::socket socket(io);
	socket.connect({asio::ip::address_v4::loopback(), address.port});
	const auto text = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n"
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
			std::array<std::vector<std::uint8_t>, 2> cards;
			check(argc <= 4, "Preview accepts at most two VMU card images");
			for (int arg = 2; arg < argc; ++arg)
			{
				std::ifstream input(argv[arg], std::ios::binary);
				auto& card = cards[arg - 2];
				card.assign(std::istreambuf_iterator<char>(input), {});
				check(card.size() == 131072, "Preview card must be a 128 KiB image");
				phone_vmu::publishCard(arg - 2, card.data(), card.size());
			}
			std::atomic<bool> finished{false};
			std::thread emulation([&] {
				while (!finished)
				{
					for (int arg = 2; arg < argc; ++arg)
					{
						auto& card = cards[arg - 2];
						// The preview only saves into its in-memory copies.
						phone_vmu::syncCard(arg - 2, card.data(), card.size(), true,
								[](const std::uint8_t *, std::size_t, bool) { return true; });
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			});
			std::cout << status.urls.front() << std::endl;
			std::cin.get();
			finished = true;
			emulation.join();
			phone_vmu::stop();
			return 0;
		}
		const std::string html = request(address, address.path);
		check(html.find("HTTP/1.1 200") == 0 && body(html).find("<canvas") != std::string::npos, "Pairing page unavailable");
		check(html.find("Cache-Control: no-store") != std::string::npos && html.find("frame-ancestors 'none'") != std::string::npos,
			"Pairing page missing privacy headers");
		check(body(request(address, address.path + "vmu-core.js")).find("const VMS") != std::string::npos, "VMU CPU was not served");
		check(body(request(address, address.path + "vmu-link.js")).find("class PhoneVmuLink") != std::string::npos, "VMU link client was not served");
		const std::string offerPath = address.path + "link/offer";
		const std::string answerPath = address.path + "link/answer";
		const std::string room(32, 'a');
		const std::string qrPath = address.path + "link/qr/";
		const auto qrResponse = request(address, qrPath + room);
		check(qrResponse.find("HTTP/1.1 200") == 0 && header(qrResponse, "Content-Type") == "application/json",
			"Invitation QR endpoint did not return JSON");
		const auto qrBody = body(qrResponse);
		const std::string qrUrl = "{\"url\":\"" + status.urls.front() + "#vmu-link=" + room + "\",\"size\":";
		check(qrBody.find(qrUrl) == 0, "Invitation QR did not use the first advertised LAN URL for loopback");
		const auto qrSize = std::stoul(qrBody.substr(qrUrl.size()));
		check(qrSize >= 21 && qrSize <= 177 && (qrSize - 21) % 4 == 0
				&& qrBody.find(",\"path\":\"M0 0h1v1h-1z", qrUrl.size()) != std::string::npos,
			"Invitation QR had invalid dimensions or dark-module path");
		check(body(request(address, qrPath + room, "GET", "", "", std::string::npos, "unpaired.example")) == qrBody,
			"Invitation QR encoded an unadvertised Host");
		const std::string& advertisedUrl = status.urls.back();
		const auto advertisedHost = advertisedUrl.substr(7, advertisedUrl.find('/', 7) - 7);
		check(body(request(address, qrPath + room, "GET", "", "", std::string::npos, advertisedHost))
				.find("{\"url\":\"" + advertisedUrl + "#vmu-link=" + room + "\"") == 0,
			"Invitation QR did not select the advertised Host");
		check(request(address, qrPath + std::string(32, 'A')).find("HTTP/1.1 404") == 0
				&& request(address, qrPath + room + "a").find("HTTP/1.1 404") == 0
				&& request(address, qrPath).find("HTTP/1.1 404") == 0,
			"Invitation QR accepted an invalid room code");
		const auto qrPost = request(address, qrPath + room, "POST");
		check(qrPost.find("HTTP/1.1 405") == 0 && header(qrPost, "Allow") == "GET", "Invitation QR accepted POST");
		const std::string linkHeaders = "Content-Type: application/octet-stream\r\nX-VMU-Link: " + room + "\r\nX-VMU-Client: ";
		const std::string firstBrowser = linkHeaders + std::string(32, 'b') + "\r\n";
		const std::string secondBrowser = linkHeaders + std::string(32, 'c') + "\r\n";
		const std::string thirdBrowser = linkHeaders + std::string(32, 'd') + "\r\n";
		check(request(address, offerPath, "POST", "offer", firstBrowser + "Origin: http://unpaired.example\r\n").find("HTTP/1.1 403") == 0,
			"Cross-origin page could create a VMU link");
		check(request(address, offerPath, "POST", "offer", firstBrowser).find("HTTP/1.1 200") == 0, "Could not create a VMU link");
		check(body(request(address, offerPath, "GET", "", secondBrowser)) == "offer", "Second browser did not receive the offer");
		check(request(address, answerPath, "GET", "", firstBrowser).find("HTTP/1.1 404") == 0, "Answer was visible before joining");
		check(request(address, answerPath, "POST", "answer", secondBrowser).find("HTTP/1.1 200") == 0, "Second browser could not join");
		check(request(address, answerPath, "POST", "other", thirdBrowser).find("HTTP/1.1 409") == 0, "Third browser replaced the answer");
		check(request(address, answerPath, "DELETE", "", thirdBrowser).find("HTTP/1.1 403") == 0, "Third browser cancelled the answer");
		check(request(address, answerPath, "DELETE", "", secondBrowser).find("HTTP/1.1 200") == 0, "Second browser could not cancel joining");
		check(request(address, answerPath, "GET", "", firstBrowser).find("HTTP/1.1 404") == 0, "Cancelled answer was still visible");
		check(request(address, answerPath, "POST", "retry", secondBrowser).find("HTTP/1.1 200") == 0, "Second browser could not retry joining");
		check(body(request(address, answerPath, "GET", "", firstBrowser)) == "retry", "First browser did not receive the new answer");
		check(request(address, offerPath, "DELETE", "", thirdBrowser).find("HTTP/1.1 403") == 0, "Third browser cancelled the VMU link");
		check(request(address, answerPath, "POST", "retry", secondBrowser).find("HTTP/1.1 200") == 0
				&& body(request(address, answerPath, "GET", "", firstBrowser)) == "retry",
			"Reading the answer broke idempotent signaling");
		check(request(address, answerPath, "DELETE", "", secondBrowser).find("HTTP/1.1 200") == 0,
			"Second browser could not cancel after the host read its answer");
		check(request(address, answerPath, "GET", "", firstBrowser).find("HTTP/1.1 409") == 0
				&& request(address, offerPath, "GET", "", thirdBrowser).find("HTTP/1.1 409") == 0
				&& request(address, answerPath, "POST", "other", thirdBrowser).find("HTTP/1.1 409") == 0,
			"Cancelled link allowed another answer after the host had read the first one");
		std::string newFirstBrowser = firstBrowser;
		newFirstBrowser.replace(newFirstBrowser.find(room), room.size(), std::string(32, 'e'));
		std::string newSecondBrowser = secondBrowser;
		newSecondBrowser.replace(newSecondBrowser.find(room), room.size(), std::string(32, 'e'));
		check(request(address, offerPath, "POST", "offer", newFirstBrowser).find("HTTP/1.1 200") == 0,
			"Cancelled link blocked a fresh invitation");
		check(request(address, offerPath, "DELETE", "", newFirstBrowser).find("HTTP/1.1 200") == 0, "First browser could not close the invitation");
		check(request(address, offerPath, "GET", "", newSecondBrowser).find("HTTP/1.1 404") == 0, "Closed link retained its offer");
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
		// Unchanged LCD frames must answer 304 to the client's cached ETag.
		const auto frameResponse = request(address, address.path + "frame/0");
		const std::string frameEtag = header(frameResponse, "ETag");
		check(request(address, address.path + "frame/0", "GET", "x", "If-None-Match: " + frameEtag + "\r\n").find("HTTP/1.1 304") == 0,
				"Unchanged frame was resent instead of 304");
		// Conflict resolution: a stale checkout is refused until the user
		// explicitly forces keeping the phone copy.
		const std::string forceLease(32, 'a'), otherLease(32, 'b');
		const std::string forceHeaders = "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + forceLease + "\r\nIf-Match: \"stale\"\r\n";
		check(request(address, address.path + "checkout/0", "POST", "x", forceHeaders).find("HTTP/1.1 409") == 0,
				"Stale checkout was accepted without force");
		check(request(address, address.path + "checkout/0", "POST", "x", forceHeaders + "X-VMU-Force: 1\r\n").find("HTTP/1.1 200") == 0,
				"Forced conflict resolution was refused");
		const std::string operation(32, 'c'), pendingCard(131072, '\x66');
		const std::string pendingHeaders = "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + forceLease
				+ "\r\nX-VMU-Operation: " + operation + "\r\n";
		check(request(address, address.path + "sync/0", "POST", pendingCard, pendingHeaders).find("HTTP/1.1 202") == 0,
				"Pending phone data was not queued before release");
		const std::string releasePath = address.path + "release/0";
		check(request(address, releasePath, "POST", "x", "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + otherLease + "\r\n").find("HTTP/1.1 409") == 0,
				"A different phone released the active lease");
		check(request(address, releasePath, "POST", "x", "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + forceLease + "\r\n").find("HTTP/1.1 200") == 0,
				"The active phone could not release its VMU");
		check(body(request(address, address.path + "card/0")) == copy,
				"Discarding a phone copy changed the desktop card bytes");
		check(request(address, address.path + "sync/0", "GET", "", pendingHeaders).find("HTTP/1.1 409") == 0,
				"Release left a queued phone operation owned or retryable");
		const std::string reopenLease(32, 'd');
		const auto reopenedRevision = header(request(address, address.path + "card/0"), "ETag");
		check(request(address, address.path + "checkout/0", "POST", "x", "Content-Type: application/octet-stream\r\nX-VMU-Lease: "
				+ reopenLease + "\r\nIf-Match: " + reopenedRevision + "\r\n").find("HTTP/1.1 200") == 0,
				"Released desktop card could not be opened again");
		check(request(address, releasePath, "POST", "x", "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + reopenLease + "\r\n").find("HTTP/1.1 200") == 0,
				"Reopened card could not release cleanly");

		std::vector<std::uint8_t> rotationCard(131072, 0x55);
		phone_vmu::publishCard(6, rotationCard.data(), rotationCard.size());
		const std::string rotationCheckout = address.path + "checkout/6", rotationSync = address.path + "sync/6";
		const auto rotationRevision = header(request(address, address.path + "card/6"), "ETag");
		const auto leaseHeaders = [](const std::string& lease) {
			return "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + lease + "\r\n";
		};
		const auto rotationHeaders = [&](const std::string& lease, const std::string& previous) {
			return leaseHeaders(lease) + "X-VMU-Previous-Lease: " + previous + "\r\n";
		};
		const std::string operationHeaders = "X-VMU-Operation: " + operation + "\r\nX-VMU-Return: 1\r\n";
		check(request(address, rotationCheckout, "POST", "x", rotationHeaders(forceLease, otherLease)
				+ "X-VMU-Force: 1\r\n").find("HTTP/1.1 409") == 0, "Lease rotation claimed an unowned card");
		check(request(address, rotationCheckout, "POST", "x", rotationHeaders(forceLease, "")
				+ "If-Match: " + rotationRevision + "\r\n").find("HTTP/1.1 403") == 0, "Checkout accepted an empty previous lease");
		check(request(address, rotationCheckout, "POST", "x", leaseHeaders(forceLease)
				+ "If-Match: " + rotationRevision + "\r\n").find("HTTP/1.1 200") == 0, "Lease rotation fixture could not check out");
		check(request(address, rotationSync, "POST", pendingCard, leaseHeaders(forceLease) + operationHeaders)
				.find("HTTP/1.1 202") == 0, "Lease rotation fixture could not queue a return");
		const auto rotatePending = rotationHeaders(otherLease, forceLease);
		check(request(address, rotationCheckout, "POST", "x", rotatePending).find("HTTP/1.1 200") == 0
				&& request(address, rotationCheckout, "POST", "x", rotatePending).find("HTTP/1.1 200") == 0,
			"Pending return lease could not rotate or retry after a lost response");
		check(request(address, rotationCheckout, "POST", "x", rotationHeaders(reopenLease, forceLease))
				.find("HTTP/1.1 409") == 0, "A competing rotation succeeded with the stale lease");
		check(request(address, rotationSync, "POST", pendingCard, leaseHeaders(forceLease) + operationHeaders)
				.find("HTTP/1.1 409") == 0, "The old lease could still write after rotation");
		check(request(address, rotationSync, "GET", "", leaseHeaders(otherLease) + operationHeaders)
				.find("HTTP/1.1 202") == 0, "Lease rotation lost the pending operation");
		check(header(request(address, address.path + "card/6"), "ETag") == rotationRevision,
			"Lease rotation changed the desktop revision");
		int writes = 0;
		const auto persistRotation = [&](const std::uint8_t *bytes, std::size_t size, bool backup) {
			++writes;
			check(backup && size == pendingCard.size() && std::memcmp(bytes, pendingCard.data(), size) == 0,
				"Lease rotation changed the pending bytes or first-write backup");
			return true;
		};
		check(phone_vmu::syncCard(6, rotationCard.data(), rotationCard.size(), true, persistRotation)
				&& !phone_vmu::syncCard(6, rotationCard.data(), rotationCard.size(), true, persistRotation) && writes == 1,
			"Rotated lease did not save its queued return and reattach the card exactly once");
		check(request(address, rotationCheckout, "POST", "x", rotationHeaders(reopenLease, otherLease))
				.find("HTTP/1.1 200") == 0, "A completed return could not rotate its lease");
		check(request(address, rotationSync, "GET", "", leaseHeaders(reopenLease) + operationHeaders).find("HTTP/1.1 200") == 0
				&& request(address, rotationSync, "POST", pendingCard, leaseHeaders(reopenLease) + operationHeaders)
						.find("HTTP/1.1 200") == 0
				&& !phone_vmu::syncCard(6, rotationCard.data(), rotationCard.size(), true, persistRotation) && writes == 1,
			"Rotated lease could not replay its completed return without another write");
		phone_vmu::unpublishCard(6, rotationCard.data());

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
		// A restart must keep the pairing address so paired phones reconnect.
		check(restarted.path == address.path, "Restart changed the pairing address");
		check(body(request(restarted, restarted.path + "frame/7"))[0] == 1, "Pairing mid-game lost the latest LCD");
		// Only an explicit regenerate revokes previously paired phones.
		const std::string rotated = phone_vmu::regeneratePairing();
		check(rotated.size() == 32 && phone_vmu::pairingToken() == rotated, "Pairing was not regenerated");
		const auto regenerated = parse(phone_vmu::status().urls.front());
		check(regenerated.path != address.path, "Regenerate reused the old pairing token");
		check(request(regenerated, address.path).find("HTTP/1.1 404") == 0, "Old pairing link remained authorized after regenerate");
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
