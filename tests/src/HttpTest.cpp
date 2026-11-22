#include "gtest/gtest.h"
#include "oslib/http_client.h"
#include <cstdio>

#ifndef _WIN32
#include "oslib/http_server.h"

#include <optional>
#include <strings.h>
#include <thread>

namespace
{
std::string header(const Request& request, const char *name)
{
	for (const Header& header : request.headers)
		if (strcasecmp(header.name.c_str(), name) == 0)
			return header.value;
	return {};
}

class CaptureServer
{
	asio::io_context io;
	HttpServer server{io, "127.0.0.1", 0};
	std::optional<Request> received;
	std::thread thread;

public:
	CaptureServer()
	{
		server.addUriHandler("/capture", [this](const Request& request, Reply& reply) {
			received = request;
			reply.setContent("received");
		});
		thread = std::thread([this] { io.run(); });
	}

	~CaptureServer() { stop(); }

	std::string url() const { return "http://127.0.0.1:" + std::to_string(server.port()) + "/capture"; }
	Request expectRequest(const char *method)
	{
		stop();
		if (!received)
		{
			ADD_FAILURE() << "The loopback server did not receive a request";
			return {};
		}
		EXPECT_EQ(method, received->method);
		EXPECT_EQ(0u, header(*received, "User-Agent").find("Flycast/"));
		return std::move(*received);
	}

private:
	void stop()
	{
		io.stop();
		if (thread.joinable())
			thread.join();
	}
};

class HttpTest : public ::testing::Test
{
protected:
	static void SetUpTestSuite() { http::init(); }
	static void TearDownTestSuite() { http::term(); }
};
}

TEST_F(HttpTest, get)
{
	CaptureServer server;
	std::vector<u8> content;
	std::string contentType;
	ASSERT_EQ(200, http::get(server.url(), content, contentType));
	EXPECT_EQ("received", std::string(content.begin(), content.end()));
	EXPECT_EQ("text/plain", contentType);
	server.expectRequest("GET");
}

TEST_F(HttpTest, getHeaders)
{
	CaptureServer server;
	std::vector<u8> content;
	const http::Headers requestHeaders = { { "Test-Header", "test-value" } };
	http::Headers responseHeaders;
	ASSERT_EQ(200, http::get(server.url(), content, &requestHeaders, &responseHeaders));
	EXPECT_NE(responseHeaders.end(), std::find(responseHeaders.begin(), responseHeaders.end(),
			std::make_pair(std::string("content-type"), std::string("text/plain"))));
	EXPECT_EQ("test-value", header(server.expectRequest("GET"), "Test-Header"));
}

TEST_F(HttpTest, postFields)
{
	CaptureServer server;
	std::vector<http::PostField> fields;
	fields.emplace_back("field1", "value1");
	fields.emplace_back("field2", "value2");
	fields.emplace_back("fileField", FLYCAST_TEST_FILES "/test_cues/d/cs.cue", "text/plain");
	ASSERT_EQ(200, http::post(server.url(), fields));
	const Request request = server.expectRequest("POST");
	EXPECT_EQ(0u, header(request, "Content-Type").find("multipart/form-data; boundary="));
	EXPECT_NE(request.content.npos, request.content.find("name=\"field1\"\r\n\r\nvalue1\r\n"));
	EXPECT_NE(request.content.npos, request.content.find("name=\"field2\"\r\n\r\nvalue2\r\n"));
	EXPECT_NE(request.content.npos, request.content.find("name=\"fileField\"; filename=\"cs.cue\""));
	EXPECT_NE(request.content.npos, request.content.find("REM SINGLE-DENSITY AREA"));
}

TEST_F(HttpTest, postContent)
{
	CaptureServer server;
	std::vector<u8> content;
	ASSERT_EQ(200, http::post(server.url(), "This is the payload", "text/plain; charset=x-sjis", content));
	EXPECT_EQ("received", std::string(content.begin(), content.end()));
	const Request request = server.expectRequest("POST");
	EXPECT_EQ("text/plain; charset=x-sjis", header(request, "Content-Type"));
	EXPECT_EQ("This is the payload", request.content);
}
#endif // #ifndef _WIN32

TEST(UrlCodecTest, Encode)
{
	EXPECT_EQ("", http::urlEncode(""));
	EXPECT_EQ("Hello%20World", http::urlEncode("Hello World"));
	EXPECT_EQ("1.2-57_alpha~", http::urlEncode("1.2-57_alpha~"));
	EXPECT_EQ("%3Chtml%20value%3D%221%22%3E", http::urlEncode("<html value=\"1\">"));

	EXPECT_EQ("%21%22%23%24%25%26%27%28%29%2A%2B%2C-.%2F", http::urlEncode("!\"#$%&'()*+,-./"));
	EXPECT_EQ("0123456789", http::urlEncode("0123456789"));
	EXPECT_EQ("%3A%3B%3C%3D%3E%3F%40", http::urlEncode(":;<=>?@"));
	EXPECT_EQ("ABCDEFGHIJKLMNOPQRSTUVWXYZ", http::urlEncode("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
	EXPECT_EQ("%5B%5C%5D%5E_%60", http::urlEncode("[\\]^_`"));
	EXPECT_EQ("abcdefghijklmnopqrstuvwxyz", http::urlEncode("abcdefghijklmnopqrstuvwxyz"));
	EXPECT_EQ("%7B%7C%7D~%7F", http::urlEncode("{|}~\x7f"));
}

TEST(UrlCodecTest, Decode)
{
	EXPECT_EQ("", http::urlDecode(""));
	EXPECT_EQ("0123456789", http::urlDecode("0123456789"));
	EXPECT_EQ("ABCDEFGHIJKLMNOPQRSTUVWXYZ", http::urlDecode("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
	EXPECT_EQ("abcdefghijklmnopqrstuvwxyz", http::urlDecode("abcdefghijklmnopqrstuvwxyz"));
	EXPECT_EQ("!\"#$%&'()*+,-./", http::urlDecode("%21%22%23%24%25%26%27%28%29%2A%2B%2C%2D%2E%2F"));
	for (unsigned c = 0x20; c <= 0x7f; ++c)
	{
		char cs[4];
		std::snprintf(cs, sizeof(cs), "%%%02X", c);
		EXPECT_EQ(std::string(1, static_cast<char>(c)), http::urlDecode(cs));
	}
	EXPECT_EQ("Hello World", http::urlDecode("Hello+World"));

	// not encoded
	EXPECT_EQ("!\"#$&'()*,-./", http::urlDecode("!\"#$&'()*,-./"));
	EXPECT_EQ(":;<=>?@[\\]^_`{|}~\x7f", http::urlDecode(":;<=>?@[\\]^_`{|}~\x7f"));
}
