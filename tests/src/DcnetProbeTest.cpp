// Portions Copyright 2026 The Hollycast Authors
#include <gtest/gtest.h>
#include "ui/dcnet_probe.h"
#include <asio.hpp>
#include <array>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace
{
using Bytes = std::vector<uint8_t>;
using Socket = asio::ip::tcp::socket;

class Server
{
	asio::io_context io;
	asio::ip::tcp::acceptor acceptor;
	Socket socket;
public:
	explicit Server(std::function<void(Socket&)> serve)
		: acceptor(io, {asio::ip::tcp::v4(), 0}), socket(io), port(acceptor.local_endpoint().port()),
		  worker([this, serve] {
			try { acceptor.accept(socket); serve(socket); }
			catch (const std::exception&) { }
		  }) {}
	~Server() { worker.join(); }
	const unsigned short port;
private:
	std::thread worker;
};

void write(Socket& socket, const Bytes& data)
{
	asio::write(socket, asio::buffer(data));
}

uint8_t readByte(Socket& socket)
{
	uint8_t byte;
	asio::read(socket, asio::buffer(&byte, 1));
	return byte;
}

uint16_t crc16(const Bytes& data, bool ppp)
{
	uint16_t crc = ppp ? 0xffff : 0;
	for (uint8_t byte : data)
	{
		crc ^= ppp ? byte : uint16_t(byte) << 8;
		for (int bit = 0; bit < 8; ++bit)
			crc = ppp ? (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0)
					: (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
	}
	return ppp ? crc ^ 0xffff : crc;
}

Bytes rallyReply(bool corrupt = false, unsigned type = 501)
{
	Bytes payload(138);
	payload[24] = type;
	payload[25] = type >> 8;
	payload[40] = 0x70; // Exercise escaping in the server response.
	const auto crc = crc16(payload, false);
	payload[20] = crc ^ (corrupt ? 1 : 0);
	payload[21] = crc >> 8;
	Bytes frame{0x70, 0x60};
	for (uint8_t byte : payload)
	{
		frame.push_back(byte);
		if (byte == 0x70)
			frame.push_back(byte);
	}
	frame.insert(frame.end(), {0x70, 0x61});
	return frame;
}

Bytes readRallyHello(Socket& socket)
{
	Bytes request(30); // 26 bytes, plus start and end markers; hello needs no escaping.
	asio::read(socket, asio::buffer(request));
	return request;
}

Bytes pppFrame(unsigned protocol, Bytes data)
{
	data.insert(data.begin(), {0xff, 3, uint8_t(protocol >> 8), uint8_t(protocol)});
	const auto crc = crc16(data, true);
	data.push_back(crc);
	data.push_back(crc >> 8);
	Bytes frame{0x7e};
	for (uint8_t byte : data)
	{
		if (byte < 32 || byte == 0x7d || byte == 0x7e)
		{
			frame.push_back(0x7d);
			byte ^= 0x20;
		}
		frame.push_back(byte);
	}
	frame.push_back(0x7e);
	return frame;
}

std::pair<unsigned, Bytes> readPpp(Socket& socket)
{
	Bytes raw;
	bool escaped = false;
	for (;;)
	{
		const auto byte = readByte(socket);
		if (byte == 0x7e)
		{
			if (raw.empty())
				continue;
			EXPECT_GE(raw.size(), 6u);
			if (raw.size() < 6)
				throw std::runtime_error("Bad test PPP frame");
			const auto crc = raw[raw.size() - 2] | (raw.back() << 8);
			raw.resize(raw.size() - 2);
			EXPECT_EQ(crc16(raw, true), crc);
			const unsigned protocol = (raw[2] << 8) | raw[3];
			return {protocol, Bytes(raw.begin() + 4, raw.end())};
		}
		if (byte == 0x7d)
			escaped = true;
		else
		{
			raw.push_back(escaped ? byte ^ 0x20 : byte);
			escaped = false;
		}
		if (raw.size() > 4096)
			throw std::runtime_error("Oversized test PPP frame");
	}
}

void control(Socket& socket, unsigned protocol, uint8_t code, uint8_t id, Bytes payload = {})
{
	const size_t length = payload.size() + 4;
	payload.insert(payload.begin(), {code, id, uint8_t(length >> 8), uint8_t(length)});
	write(socket, pppFrame(protocol, payload));
}

// Deliberately exercise EAP and IPCP negotiation before answering the game query.
void negotiatePba(Socket& socket)
{
	const auto lcp = readPpp(socket);
	EXPECT_EQ(lcp.first, 0xc021u);
	EXPECT_EQ(lcp.second, (Bytes{1, 1, 0, 4}));
	control(socket, 0xc021, 1, 9, {3, 4, 0xc2, 0x27});
	EXPECT_EQ(readPpp(socket).second, (Bytes{2, 9, 0, 8, 3, 4, 0xc2, 0x27}));
	control(socket, 0xc021, 2, 1);
	control(socket, 0xc227, 1, 1, {1});
	EXPECT_EQ(readPpp(socket).second, (Bytes{2, 1, 0, 13, 1, 'f','l','y','c','a','s','t','1'}));
	control(socket, 0xc227, 1, 2, {4, 4, 1, 2, 3, 4});
	const auto md5 = readPpp(socket);
	EXPECT_EQ(md5.first, 0xc227u);
	ASSERT_EQ(md5.second.size(), 30u);
	EXPECT_EQ(Bytes(md5.second.begin() + 6, md5.second.begin() + 22), (Bytes{0x76,0x70,0xf0,0x45,0xc6,0x41,0x7f,0x0f,0x1b,0xf0,0x16,0x38,0x3a,0xba,0xe7,0x30}));
	control(socket, 0xc227, 3, 2);
	const auto ip = readPpp(socket);
	EXPECT_EQ(ip.first, 0x8021u);
	control(socket, 0x8021, 1, 8, {3, 6, 172, 20, 0, 1});
	EXPECT_EQ(readPpp(socket).second[0], 2);
	control(socket, 0x8021, 3, ip.second[1], {3, 6, 172, 20, 1, 2, 129, 6, 172, 20, 0, 1});
	auto assigned = readPpp(socket).second;
	assigned[0] = 2;
	write(socket, pppFrame(0x8021, assigned));
}

uint16_t internetChecksum(const Bytes& data)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < data.size(); i += 2)
		sum += (data[i] << 8) | (i + 1 < data.size() ? data[i + 1] : 0);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

void put16(Bytes& data, size_t pos, unsigned value)
{
	data.at(pos) = value >> 8;
	data.at(pos + 1) = value;
}

void put32(Bytes& data, size_t pos, uint32_t value)
{
	put16(data, pos, value >> 16);
	put16(data, pos + 2, value);
}

uint32_t get32(const Bytes& data, size_t pos)
{
	return (uint32_t(data.at(pos)) << 24) | (uint32_t(data.at(pos + 1)) << 16)
			| (data.at(pos + 2) << 8) | data.at(pos + 3);
}

void sendIp(Socket& socket, uint8_t protocol, Bytes payload)
{
	Bytes ip(20);
	ip[0] = 0x45;
	put16(ip, 2, 20 + payload.size());
	ip[8] = 64;
	ip[9] = protocol;
	put32(ip, 12, 0xac140001);
	put32(ip, 16, 0xac140102);
	put16(ip, 10, internetChecksum(ip));
	ip.insert(ip.end(), payload.begin(), payload.end());
	write(socket, pppFrame(0x21, ip));
}

void servePba(Socket& socket, const std::string& lobby, bool corrupt = false)
{
	negotiatePba(socket);
	const auto dns = readPpp(socket);
	EXPECT_EQ(dns.first, 0x21u);
	EXPECT_EQ(dns.second.at(9), 17);
	Bytes query(dns.second.begin() + 28, dns.second.end());
	EXPECT_EQ(std::string(query.begin() + 13, query.begin() + 19), "master");
	query[2] = 0x81;
	query[3] = 0x80;
	query[7] = 1;
	query.insert(query.end(), {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 172, 20, 0, 1});
	Bytes udp{0, 53, 0xcf, 8, 0, 0, 0, 0};
	put16(udp, 4, query.size() + 8);
	udp.insert(udp.end(), query.begin(), query.end());
	sendIp(socket, 17, udp);
	const auto syn = readPpp(socket).second;
	uint32_t ack = get32(syn, 24) + 1, seq = 7000;
	auto sendTcp = [&](uint8_t flags, const std::string& text = std::string()) {
		Bytes tcp(20);
		put16(tcp, 0, 28900);
		put16(tcp, 2, 55021);
		put32(tcp, 4, seq);
		put32(tcp, 8, ack);
		tcp[12] = 0x50;
		tcp[13] = flags;
		put16(tcp, 14, 8192);
		tcp.insert(tcp.end(), text.begin(), text.end());
		Bytes pseudo(12);
		put32(pseudo, 0, 0xac140001);
		put32(pseudo, 4, 0xac140102);
		pseudo[9] = 6;
		put16(pseudo, 10, tcp.size());
		pseudo.insert(pseudo.end(), tcp.begin(), tcp.end());
		put16(tcp, 16, internetChecksum(pseudo) ^ (corrupt && text == lobby ? 1 : 0));
		sendIp(socket, 6, tcp);
		seq += text.size();
	};
	sendTcp(0x12);
	++seq;
	readPpp(socket); // SYN ACK
	sendTcp(0x18, "\\basic\\\\secure\\ABCDEF");
	readPpp(socket); // challenge ACK
	const auto request = readPpp(socket).second;
	const std::string text(request.begin() + 40, request.end());
	EXPECT_EQ(text, "\\gamename\\pba2001\\enctype\\0\\validate\\UvdQdxyG\\final\\\\list\\groups\\gamename\\pba2001\\final\\");
	ack += text.size();
	sendTcp(0x18, lobby);
	readPpp(socket); // reply ACK
}
}

TEST(DcnetProbe, SegaRallyAcceptsNativeReplyAndSendsInitializationOnly)
{
	Server server([](Socket& socket) {
		const auto hello = readRallyHello(socket);
		EXPECT_EQ(hello[26], 566 & 255);
		EXPECT_EQ(hello[27], 566 >> 8);
		write(socket, {'\r','\n','C','O','N','N','E','C','T',' ','3','3','6','0','0','\r','\n'});
		const auto reply = rallyReply();
		// TCP segmentation must not change the result.
		write(socket, Bytes(reply.begin(), reply.begin() + 27));
		write(socket, Bytes(reply.begin() + 27, reply.end()));
	});
	EXPECT_TRUE(dcnet::probeSegaRally("127.0.0.1", server.port));
}

TEST(DcnetProbe, SegaRallyRejectsCorruptChecksum)
{
	Server server([](Socket& socket) { readRallyHello(socket); write(socket, rallyReply(true)); });
	EXPECT_FALSE(dcnet::probeSegaRally("127.0.0.1", server.port));
}

TEST(DcnetProbe, SegaRallyRejectsUnrelatedReply)
{
	Server server([](Socket& socket) { readRallyHello(socket); write(socket, rallyReply(false, 502)); });
	EXPECT_FALSE(dcnet::probeSegaRally("127.0.0.1", server.port));
}

TEST(DcnetProbe, SegaRallyRejectsTruncatedReply)
{
	Server server([](Socket& socket) {
		readRallyHello(socket);
		auto reply = rallyReply();
		reply.resize(20);
		write(socket, reply);
	});
	EXPECT_FALSE(dcnet::probeSegaRally("127.0.0.1", server.port));
}

TEST(DcnetProbe, SegaRallyStopsAtDeadline)
{
	Server server([](Socket& socket) {
		readRallyHello(socket);
		// Wait for the probe to close, rather than sleeping in a test.
		readByte(socket);
	});
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(dcnet::probeSegaRally("127.0.0.1", server.port, std::chrono::milliseconds(50)));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(DcnetProbe, PbaAcceptsItsLobbyThroughVpn)
{
	Server server([](Socket& socket) {
		servePba(socket, "\\fieldcount\\7\\groupid\\hostname\\numplayers\\maxwaiting\\numwaiting\\numservers\\password\\100\\Lobby\\0\\50\\0\\0\\0\\final\\");
	});
	EXPECT_TRUE(dcnet::probePba("127.0.0.1", server.port));
}

TEST(DcnetProbe, PbaRejectsOtherMasterResponses)
{
	Server server([](Socket& socket) { servePba(socket, "\\error\\Unknown game\\final\\"); });
	EXPECT_FALSE(dcnet::probePba("127.0.0.1", server.port));
}

TEST(DcnetProbe, PbaRejectsMalformedNegotiation)
{
	Server server([](Socket& socket) {
		readPpp(socket);
		control(socket, 0xc021, 1, 9, {3, 255});
	});
	EXPECT_FALSE(dcnet::probePba("127.0.0.1", server.port));
}

TEST(DcnetProbe, PbaStopsAtDeadline)
{
	Server server([](Socket& socket) { readPpp(socket); readByte(socket); });
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(dcnet::probePba("127.0.0.1", server.port, std::chrono::milliseconds(50)));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(DcnetProbe, PbaRejectsInvalidTcpChecksum)
{
	Server server([](Socket& socket) {
		servePba(socket, "\\fieldcount\\7\\groupid\\hostname\\numplayers\\maxwaiting\\numwaiting\\numservers\\password\\100\\Lobby\\0\\50\\0\\0\\0\\final\\", true);
	});
	EXPECT_FALSE(dcnet::probePba("127.0.0.1", server.port));
}
