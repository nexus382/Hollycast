// Portions Copyright 2026 The Hollycast Authors
#include "oslib/http_client.h"
#include <gtest/gtest.h>
#include <asio.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace
{
// Timeout cases deliberately leave a response unfinished. The independent deadline
// also lets those regressions fail cleanly if the client stops applying timeouts.
class StalledServer
{
	asio::io_context io;
	asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
	asio::ip::tcp::socket socket{io};
	asio::streambuf request;
	asio::steady_timer deadline{io};
	std::string response;
	bool closeAfterResponse = false;
	std::mutex responseMutex;
	std::condition_variable responseCondition;
	bool responseSent = false;
	std::thread worker;
public:
	explicit StalledServer(bool sendHeaders)
		: StalledServer(sendHeaders ? "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nx" : "", false)
	{
	}
	StalledServer(const std::string& responseBytes, bool closeWhenSent)
		: response(responseBytes), closeAfterResponse(closeWhenSent)
	{
		acceptor.async_accept(socket, [this](const asio::error_code& ec) {
			if (ec || response.empty()) return;
			asio::async_read_until(socket, request, "\r\n\r\n", [this](const asio::error_code& readError, size_t) {
				if (readError) return;
				asio::async_write(socket, asio::buffer(response), [this](const asio::error_code& writeError, size_t) {
					{
						std::lock_guard<std::mutex> lock(responseMutex);
						responseSent = !writeError;
					}
					responseCondition.notify_all();
					if (closeAfterResponse)
					{
						asio::error_code ignored;
						socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
						socket.close(ignored);
					}
				});
			});
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
	bool waitForResponse(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(responseMutex);
		return responseCondition.wait_for(lock, timeout, [this] { return responseSent; });
	}
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

TEST_F(HttpTimeoutTest, AcceptsResponseAtExactByteLimit)
{
	const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nabcd";
	StalledServer server(response, true);
	std::vector<u8> content;
	EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 500, 4), 200);
	EXPECT_EQ(content, (std::vector<u8>{'a', 'b', 'c', 'd'}));
}

TEST_F(HttpTimeoutTest, RejectsAdvertisedResponseLargerThanByteLimit)
{
	const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nabcdef";
	StalledServer server(response, true);
	std::vector<u8> content;
	EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 500, 4), 413);
	EXPECT_TRUE(content.empty());
}

TEST_F(HttpTimeoutTest, RejectsChunkedResponseLargerThanByteLimit)
{
	const std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n";
	StalledServer server(response, true);
	std::vector<u8> content;
	EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 500, 4), 413);
	EXPECT_TRUE(content.empty());
}

TEST_F(HttpTimeoutTest, CancelsBeforeMakingTheRequest)
{
	const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nabcd";
	StalledServer server(response, true);
	std::atomic_bool cancelled{true};
	std::vector<u8> content;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_EQ(http::get(server.url(), content, nullptr, nullptr, 500, 4, &cancelled), 499);
	EXPECT_TRUE(content.empty());
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST_F(HttpTimeoutTest, CancelsWhileWaitingForTheResponseBody)
{
	const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nx";
	StalledServer server(response, false);
	std::atomic_bool cancelled{false};
	std::vector<u8> content;
	const auto start = std::chrono::steady_clock::now();
	auto request = std::async(std::launch::async, [&] {
		return http::get(server.url(), content, nullptr, nullptr, 500, 10, &cancelled);
	});
	ASSERT_TRUE(server.waitForResponse(std::chrono::seconds(2))) << "The local server did not send the partial body";
	ASSERT_EQ(request.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout)
			<< "The request completed before the partial body was cancelled";
	cancelled.store(true);
	ASSERT_EQ(request.wait_for(std::chrono::seconds(9)), std::future_status::ready)
			<< "Cancellation exceeded the documented WinINet timeout tolerance";
	EXPECT_EQ(request.get(), 499);
	EXPECT_TRUE(content.empty());
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(9));
}
