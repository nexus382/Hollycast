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

class TinyHttpServer
{
	asio::io_context io;
	asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
	std::string response;
	std::thread worker;
public:
	explicit TinyHttpServer(std::string response) : response(std::move(response))
	{
		worker = std::thread([this] {
			asio::ip::tcp::socket socket(io);
			acceptor.accept(socket);
			asio::error_code ec;
			std::string request;
			asio::read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", ec);
			if (!ec) asio::write(socket, asio::buffer(this->response), ec);
		});
	}
	~TinyHttpServer() { io.stop(); acceptor.close(); worker.join(); }
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

TEST(HttpBodyLimitTest, StopsAtLimitWithoutContentLength)
{
	http::init();
	TinyHttpServer server("HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n12345");
	std::vector<u8> content;
	EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 1000, nullptr, 4), http::bodyTooLarge);
	EXPECT_TRUE(content.empty());
	http::term();
}

TEST(HttpBodyLimitTest, AcceptsExactLimitAndRejectsOneExtraByte)
{
	http::init();
	{
		TinyHttpServer server("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\n1234");
		std::vector<u8> content;
		EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 1000, nullptr, 4), 200);
		EXPECT_EQ(std::string(content.begin(), content.end()), "1234");
	}
	{
		TinyHttpServer server("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\n12345");
		std::vector<u8> content;
		EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 1000, nullptr, 4), http::bodyTooLarge);
		EXPECT_TRUE(content.empty());
	}
	http::term();
}

TEST(HttpBodyLimitTest, LimitsChunkedBodiesAndKeepsUnlimitedCalls)
{
	http::init();
	{
		TinyHttpServer server("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n4\r\n1234\r\n1\r\n5\r\n0\r\n\r\n");
		std::vector<u8> content;
		EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 1000, nullptr, 4), http::bodyTooLarge);
		EXPECT_TRUE(content.empty());
	}
	{
		TinyHttpServer server("HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n12345");
		std::vector<u8> content;
		EXPECT_EQ(http::get(server.url(), content), 200);
		EXPECT_EQ(content.size(), 5u);
	}
	http::term();
}

TEST(HttpBodyLimitTest, HonorsCancellationBeforeStarting)
{
	http::init();
	std::atomic<bool> cancelled{true};
	std::vector<u8> content;
	EXPECT_EQ(http::get("http://127.0.0.1:1/", content, nullptr, nullptr, 1000, &cancelled, 4), 499);
	EXPECT_TRUE(content.empty());
	http::term();
}

TEST(HttpBodyLimitTest, CancelsPartialResponseAndClearsBody)
{
	http::init();
	StalledServer server(true);
	std::atomic<bool> cancelled{false};
	std::thread cancelRequest([&cancelled] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		cancelled.store(true);
	});
	std::vector<u8> content;
	EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 0, &cancelled, 10), 499);
	cancelRequest.join();
	EXPECT_TRUE(content.empty());
	http::term();
}
