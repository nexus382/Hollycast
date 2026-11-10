// Portions Copyright 2026 The Hollycast Authors
#include "oslib/http_client.h"
#include <gtest/gtest.h>
#include <asio.hpp>
#include <chrono>
#include <thread>

namespace
{
// The server deliberately leaves a response unfinished. Its independent deadline
// also lets the regression fail cleanly if the client stops applying timeouts.
class StalledServer
{
	asio::io_context io;
	asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
	asio::ip::tcp::socket socket{io};
	asio::steady_timer deadline{io};
	std::string response;
	std::thread worker;
public:
	explicit StalledServer(bool sendHeaders)
		: response(sendHeaders ? "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nx" : "")
	{
		acceptor.async_accept(socket, [this](const asio::error_code& ec) {
			if (!ec && !response.empty())
				asio::async_write(socket, asio::buffer(response), [](const asio::error_code&, size_t) {});
		});
		deadline.expires_after(std::chrono::seconds(12));
		deadline.async_wait([this](const asio::error_code& ec) {
			if (!ec)
			{
				asio::error_code ignored;
				socket.close(ignored);
				acceptor.close(ignored);
			}
		});
		worker = std::thread([this] { io.run(); });
	}
	~StalledServer()
	{
		io.stop();
		worker.join();
	}
	std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/"; }
};

class HttpTimeoutTest : public ::testing::Test
{
public:
	static void SetUpTestSuite() { http::init(); }
	static void TearDownTestSuite() { http::term(); }
};
}

TEST_F(HttpTimeoutTest, StopsWaitingForResponseHeaders)
{
	StalledServer server(false);
	std::vector<u8> content;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(http::success(http::get(server.url(), content, nullptr, nullptr, 100)));
	// WinINet documents up to six seconds of timeout scheduling tolerance.
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(9));
}

TEST_F(HttpTimeoutTest, StopsWaitingForAnIncompleteResponseBody)
{
	StalledServer server(true);
	std::vector<u8> content;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(http::success(http::get(server.url(), content, nullptr, nullptr, 100)));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(9));
}
