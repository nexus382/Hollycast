// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "phone_vmu.h"

#ifdef HAVE_PHONE_VMU
#include "phone_vmu_page.h"
#include "phone_vmu_player.h"
#include "oslib/http_server.h"
#include "rend/vmu_frame_store.h"
#include "deps/vmu-softvms/core.h"
#include "deps/nosleep/media.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace phone_vmu
{
namespace
{
using Clock = std::chrono::steady_clock;

std::int64_t nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string sessionToken()
{
	std::random_device random;
	std::uniform_int_distribution<int> byte(0, 255);
	std::string token;
	for (int i = 0; i < 16; ++i)
	{
		int value = byte(random);
		token += "0123456789abcdef"[value >> 4];
		token += "0123456789abcdef"[value & 15];
	}
	return token;
}

std::vector<std::string> localAddresses()
{
	std::vector<std::string> addresses;
	auto add = [&](const sockaddr_in& address) {
		const asio::ip::address_v4 ip(ntohl(address.sin_addr.s_addr));
		if (!ip.is_loopback() && !ip.is_unspecified() && !ip.is_multicast())
			addresses.push_back(ip.to_string());
	};
#ifdef _WIN32
	asio::io_context io;
	asio::ip::udp::socket socket(io, asio::ip::udp::v4());
	std::array<INTERFACE_INFO, 64> interfaces;
	DWORD bytes = 0;
	if (WSAIoctl(socket.native_handle(), SIO_GET_INTERFACE_LIST, nullptr, 0,
			interfaces.data(), sizeof(interfaces), &bytes, nullptr, nullptr) == 0)
		for (std::size_t i = 0; i < bytes / sizeof(INTERFACE_INFO); ++i)
			if ((interfaces[i].iiFlags & IFF_UP) && interfaces[i].iiAddress.Address.sa_family == AF_INET)
				add(interfaces[i].iiAddress.AddressIn);
#else
	ifaddrs *interfaces = nullptr;
	if (getifaddrs(&interfaces) == 0)
	{
		for (ifaddrs *it = interfaces; it != nullptr; it = it->ifa_next)
			if (it->ifa_addr != nullptr && (it->ifa_flags & IFF_UP) && it->ifa_addr->sa_family == AF_INET)
				add(*reinterpret_cast<sockaddr_in *>(it->ifa_addr));
		freeifaddrs(interfaces);
	}
#endif
	// Prefer home LAN addresses over VPN/public/link-local interfaces.
	auto privateAddress = [](const std::string& address) {
		const auto ip = asio::ip::make_address_v4(address).to_uint();
		return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8;
	};
	std::sort(addresses.begin(), addresses.end(), [&](const auto& a, const auto& b) {
		if (privateAddress(a) != privateAddress(b))
			return privateAddress(a);
		return a < b;
	});
	addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
	return addresses;
}

constexpr std::size_t CardSize = 128 * 1024;

class CardStore
{
public:
	void publish(int index, std::size_t offset, const std::uint8_t *data, std::size_t size, bool full)
	{
		if (index < 0 || index >= vmu_window::ScreenCount || data == nullptr
				|| offset > CardSize || size > CardSize - offset || (full && size != CardSize))
			return;
		std::lock_guard<std::mutex> lock(mutex);
		auto& card = cards[index];
		if (full)
			card.resize(CardSize);
		if (card.size() == CardSize)
			std::copy_n(data, size, card.begin() + offset);
	}

	std::vector<std::uint8_t> snapshot(int index)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return cards[index];
	}

private:
	std::mutex mutex;
	std::array<std::vector<std::uint8_t>, vmu_window::ScreenCount> cards;
};

class Session
{
public:
	Session(vmu_window::FrameStore& frames, CardStore& cards)
	{
		const std::string path = "/" + sessionToken() + "/";
		server = std::make_unique<HttpServer>(io, "0.0.0.0", 0);
		for (const std::string& address : localAddresses())
			urls.push_back("http://" + address + ":" + std::to_string(server->port()) + path);
		if (urls.empty())
			throw std::runtime_error("No local network address found. Connect this computer to Wi-Fi or Ethernet and try again.");
		server->addUriHandler("/", [this, &frames, &cards, path](const Request& req, Reply& rep) {
			rep.addHeader("Cache-Control", "no-store");
			rep.addHeader("X-Content-Type-Options", "nosniff");
			rep.addHeader("Referrer-Policy", "no-referrer");
			rep.addHeader("Content-Security-Policy", "default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; media-src data:; frame-ancestors 'none'; base-uri 'none'");
			if (req.method != "GET")
			{
				rep.addHeader("Allow", "GET");
				rep.setContent("Only GET is supported.");
				rep.status = Reply::method_not_allowed;
			}
			else if (req.uri == path)
			{
				rep.setContent(page, "text/html; charset=utf-8");
			}
			else if (req.uri == path + "vmu-core.js")
				rep.setContent(standaloneCore, "text/javascript; charset=utf-8");
			else if (req.uri == path + "vmu-player.js")
				rep.setContent(standalonePlayer, "text/javascript; charset=utf-8");
			else if (req.uri == path + "vmu-core.LICENSE")
				rep.setContent(standaloneLicense, "text/plain; charset=utf-8");
			else if (req.uri == path + "wake-media.js")
				rep.setContent(wakeMedia, "text/javascript; charset=utf-8");
			else if (req.uri.size() == path.size() + 6 && req.uri.compare(0, path.size() + 5, path + "card/") == 0
					&& req.uri.back() >= '0' && req.uri.back() <= '7')
			{
				const auto card = cards.snapshot(req.uri.back() - '0');
				if (card.empty())
				{
					rep.setContent("No VMU card is available for this slot. Start a game or open a VMU game file on your phone.");
					rep.status = Reply::not_found;
				}
				else
					rep.setContent(std::string(card.begin(), card.end()), "application/octet-stream");
			}
			else if (req.uri.size() == path.size() + 7 && req.uri.compare(0, path.size() + 6, path + "frame/") == 0
					&& req.uri.back() >= '0' && req.uri.back() <= '7')
			{
				const auto frame = frames.snapshot()[req.uri.back() - '0'];
				// Send top-down rows for the browser; LCD producers use bottom-up rows.
				// A status byte followed by the 48x32 monochrome LCD, LSB first.
				std::string data(1 + vmu_window::LcdWidth * vmu_window::LcdHeight / 8, '\0');
				data[0] = frame.active ? 1 : 0;
				if (frame.active)
					for (int y = 0; y < vmu_window::LcdHeight; ++y)
						for (int x = 0; x < vmu_window::LcdWidth; ++x)
						{
							const auto pixel = frame.pixels[(vmu_window::LcdHeight - 1 - y) * vmu_window::LcdWidth + x];
							const int i = y * vmu_window::LcdWidth + x;
							if ((pixel & 0x00ffffff) == 0 && (pixel & 0xff000000) != 0)
								data[1 + i / 8] |= 1 << (i % 8);
						}
				rep.setContent(data, "application/octet-stream");
				lastFrameRequest = nowMs();
			}
			else
			{
				rep.setContent("Open the address shown in Hollycast's Connect Phone settings.");
				rep.status = Reply::not_found;
			}
		});
		running = true;
		worker = std::thread([this] {
			try { io.run(); }
			catch (const std::exception& e)
			{
				std::lock_guard<std::mutex> lock(errorMutex);
				error = e.what();
			}
			running = false;
		});
	}

	~Session()
	{
		io.stop();
		if (worker.joinable())
			worker.join();
	}

	Status status()
	{
		std::lock_guard<std::mutex> lock(errorMutex);
		return { running.load(), lastFrameRequest != 0 && nowMs() - lastFrameRequest < 3000, urls, error };
	}

private:
	// Destroy the server and all outstanding operations before the context.
	asio::io_context io;
	std::unique_ptr<HttpServer> server;
	std::thread worker;
	std::atomic<bool> running { false };
	std::atomic<std::int64_t> lastFrameRequest { 0 };
	std::vector<std::string> urls;
	std::mutex errorMutex;
	std::string error;
};

struct Service
{
	vmu_window::FrameStore frames;
	CardStore cards;
	std::unique_ptr<Session> session;
	std::string error;
};

Service& service()
{
	static Service instance;
	return instance;
}
}

void start()
{
	stop();
	try { service().session = std::make_unique<Session>(service().frames, service().cards); }
	catch (const std::exception& e) { service().error = e.what(); }
}

void stop()
{
	service().session.reset();
	service().error.clear();
}

Status status()
{
	if (service().session)
		return service().session->status();
	return { false, false, {}, service().error };
}

void publishScreen(int index, const std::uint32_t *pixels, bool active)
{
	service().frames.publish(index, pixels, active);
}

void publishCard(int index, const std::uint8_t *data, std::size_t size)
{
	service().cards.publish(index, 0, data, size, true);
}

void updateCard(int index, std::size_t offset, const std::uint8_t *data, std::size_t size)
{
	service().cards.publish(index, offset, data, size, false);
}
}
#endif
