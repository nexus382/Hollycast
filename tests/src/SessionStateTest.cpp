// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "network/session_state.h"
#include <array>
#include <chrono>
#include <future>

// Wire-format coverage for the host-to-guest session state bundle: valid
// bundles must round-trip byte-for-byte, and malformed bundles must be
// rejected without producing partial items.
TEST(SessionStateTest, RoundTripPreservesItems)
{
	std::vector<sessionstate::Item> items;
	sessionstate::Item flash;
	flash.kind = sessionstate::KIND_FLASH;
	flash.data = { 0x10, 0x20, 0x30, 0x40 };
	items.push_back(flash);
	sessionstate::Item vmu;
	vmu.kind = sessionstate::KIND_VMU;
	vmu.bus = 1;
	vmu.port = 5;
	vmu.data.assign(1024, 0xab);
	items.push_back(vmu);

	std::vector<u8> encoded;
	ASSERT_TRUE(sessionstate::encode(items, encoded));

	std::vector<sessionstate::Item> decoded;
	ASSERT_TRUE(sessionstate::decode(encoded.data(), encoded.size(), decoded));
	ASSERT_EQ(decoded.size(), 2u);
	EXPECT_EQ(decoded[0].kind, sessionstate::KIND_FLASH);
	EXPECT_EQ(decoded[0].data, flash.data);
	EXPECT_EQ(decoded[1].kind, sessionstate::KIND_VMU);
	EXPECT_EQ(decoded[1].bus, 1);
	EXPECT_EQ(decoded[1].port, 5);
	EXPECT_EQ(decoded[1].data, vmu.data);
}

TEST(SessionStateTest, RejectsMalformedBundles)
{
	std::vector<sessionstate::Item> items;
	sessionstate::Item flash;
	flash.kind = sessionstate::KIND_FLASH;
	flash.data = { 1, 2, 3 };
	items.push_back(flash);
	std::vector<u8> encoded;
	ASSERT_TRUE(sessionstate::encode(items, encoded));

	std::vector<sessionstate::Item> decoded;
	// Bad magic
	std::vector<u8> bad = encoded;
	bad[0] = 'X';
	EXPECT_FALSE(sessionstate::decode(bad.data(), bad.size(), decoded));
	// Truncated payload
	EXPECT_FALSE(sessionstate::decode(encoded.data(), encoded.size() - 1, decoded));
	// Trailing garbage
	bad = encoded;
	bad.push_back(0);
	EXPECT_FALSE(sessionstate::decode(bad.data(), bad.size(), decoded));
	// Unknown item kind
	bad = encoded;
	bad[8] = 99;
	EXPECT_FALSE(sessionstate::decode(bad.data(), bad.size(), decoded));
	EXPECT_TRUE(decoded.empty());
}

TEST(SessionStateTest, RequiresProofAndAdoptionBeforeCompletingTransfer)
{
#ifdef _WIN32
	WSADATA data{};
	ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
	struct WinsockCleanup { ~WinsockCleanup() { WSACleanup(); } } winsock;
#endif
	sockaddr_in loopback{};
	loopback.sin_family = AF_INET;
	loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const sessionstate::Socket listener(socket(AF_INET, SOCK_STREAM, 0));
	const sessionstate::Socket hostUdp(socket(AF_INET, SOCK_DGRAM, 0));
	const sessionstate::Socket guestUdp(socket(AF_INET, SOCK_DGRAM, 0));
	ASSERT_TRUE(VALID(listener.get()) && VALID(hostUdp.get()) && VALID(guestUdp.get()));
	ASSERT_EQ(bind(listener.get(), (sockaddr*)&loopback, sizeof(loopback)), 0);
	ASSERT_EQ(listen(listener.get(), 1), 0);
	ASSERT_EQ(bind(hostUdp.get(), (sockaddr*)&loopback, sizeof(loopback)), 0);
	ASSERT_EQ(bind(guestUdp.get(), (sockaddr*)&loopback, sizeof(loopback)), 0);
	sockaddr_in tcp = loopback;
	sockaddr_in host = loopback;
	sockaddr_in guest = loopback;
	socklen_t size = sizeof(loopback);
	ASSERT_EQ(getsockname(listener.get(), (sockaddr*)&tcp, &size), 0);
	ASSERT_EQ(getsockname(hostUdp.get(), (sockaddr*)&host, &size), 0);
	ASSERT_EQ(getsockname(guestUdp.get(), (sockaddr*)&guest, &size), 0);
	EXPECT_NE(host.sin_port, guest.sin_port);

	using sessionstate::ServeResult;
	using namespace std::chrono_literals;
	constexpr u32 protocol = 9;
	const std::vector<sessionstate::Item> bundle = { { sessionstate::KIND_VMU, 0, 5, { 1, 2, 3 } } };
	std::atomic<bool> stop{ false };
	const auto serve = [&](sockaddr_in& endpoint) {
		const sessionstate::Socket client(accept(listener.get(), nullptr, nullptr));
		return sessionstate::serveGuest(client.get(), hostUdp.get(), endpoint, protocol, bundle, stop);
	};
	{
		const sessionstate::Socket client(sessionstate::connectWithTimeout(tcp, 1000, stop));
		ASSERT_TRUE(VALID(client.get()));
		ASSERT_TRUE(set_recv_timeout(client.get(), 1000));
		const std::array<char, 8> request{ 'H', 'C', 'R', 'Q', static_cast<char>(protocol) };
		ASSERT_EQ(send(client.get(), request.data(), (int)request.size(), 0), (int)request.size());
		sockaddr_in rejected = loopback;
		auto server = std::async(std::launch::async, serve, std::ref(rejected));
		std::array<char, 21> proof{};
		const int received = recv(client.get(), proof.data(), (int)proof.size(), MSG_WAITALL);
		EXPECT_EQ(received, (int)proof.size());
		proof.back() ^= 1;
		EXPECT_EQ(sendto(guestUdp.get(), proof.data(), (int)proof.size(), 0, (sockaddr*)&host, sizeof(host)), (int)proof.size());
		EXPECT_FALSE(sessionstate::waitForReadable(client.get(), 150));
		stop = true;
		EXPECT_EQ(server.wait_for(1s), std::future_status::ready);
		EXPECT_EQ(server.get(), ServeResult::Retry);
		EXPECT_EQ(rejected.sin_port, 0);
	}

	stop = false;
	{
		const sessionstate::Socket client(sessionstate::connectWithTimeout(tcp, 1000, stop));
		ASSERT_TRUE(VALID(client.get()));
		const std::array<char, 8> oldRequest{ 'H', 'C', 'R', 'Q', static_cast<char>(protocol - 1) };
		ASSERT_EQ(send(client.get(), oldRequest.data(), (int)oldRequest.size(), 0), (int)oldRequest.size());
		sockaddr_in rejected = loopback;
		auto server = std::async(std::launch::async, serve, std::ref(rejected));
		EXPECT_EQ(server.wait_for(1s), std::future_status::ready);
		EXPECT_EQ(server.get(), ServeResult::IncompatibleVersion);
		EXPECT_EQ(rejected.sin_port, 0);
	}

	{
		const sessionstate::Socket client(sessionstate::connectWithTimeout(tcp, 1000, stop));
		ASSERT_TRUE(VALID(client.get()));
		sockaddr_in rejected = loopback;
		auto server = std::async(std::launch::async, serve, std::ref(rejected));
		std::vector<sessionstate::Item> items;
		EXPECT_TRUE(sessionstate::fetchFromHost(client.get(), guestUdp.get(), host, protocol, items, stop));
		// Local adoption failed. End the TCP stream without acknowledging the bundle.
		EXPECT_EQ(shutdown(client.get(), SHUT_WR), 0);
		EXPECT_EQ(server.wait_for(1s), std::future_status::ready);
		EXPECT_EQ(server.get(), ServeResult::Retry);
		EXPECT_EQ(rejected.sin_port, 0);
	}

	{
		const sessionstate::Socket client(sessionstate::connectWithTimeout(tcp, 1000, stop));
		ASSERT_TRUE(VALID(client.get()));
		sockaddr_in rejected = loopback;
		auto server = std::async(std::launch::async, serve, std::ref(rejected));
		std::vector<sessionstate::Item> items;
		EXPECT_TRUE(sessionstate::fetchFromHost(client.get(), guestUdp.get(), host, protocol, items, stop));
		EXPECT_EQ(server.wait_for(0ms), std::future_status::timeout);
		stop = true;
		EXPECT_EQ(server.wait_for(1s), std::future_status::ready);
		EXPECT_EQ(server.get(), ServeResult::Retry);
		EXPECT_EQ(rejected.sin_port, 0);
	}

	stop = false;
	const sessionstate::Socket client(sessionstate::connectWithTimeout(tcp, 1000, stop));
	ASSERT_TRUE(VALID(client.get()));
	sockaddr_in learned = loopback;
	auto server = std::async(std::launch::async, serve, std::ref(learned));
	std::vector<sessionstate::Item> items;
	EXPECT_TRUE(sessionstate::fetchFromHost(client.get(), guestUdp.get(), host, protocol, items, stop));
	EXPECT_TRUE(sessionstate::acknowledgeHost(client.get(), stop));
	EXPECT_EQ(server.get(), ServeResult::Complete);
	EXPECT_EQ(learned.sin_port, guest.sin_port);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items.front().data, bundle.front().data);
}
