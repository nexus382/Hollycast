// Copyright 2004-2008 Luigi Auriemma (GameSpy challenge-response algorithm)
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "dcnet_probe.h"
#include "deps/md5/md5.h"
#include <asio.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dcnet
{
namespace
{
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

uint16_t be16(const Bytes& data, size_t pos)
{
	return (data.at(pos) << 8) | data.at(pos + 1);
}

uint32_t be32(const Bytes& data, size_t pos)
{
	return (uint32_t(be16(data, pos)) << 16) | be16(data, pos + 2);
}

void put16(Bytes& data, size_t pos, uint16_t value)
{
	data.at(pos) = value >> 8;
	data.at(pos + 1) = value;
}

void put32(Bytes& data, size_t pos, uint32_t value)
{
	put16(data, pos, value >> 16);
	put16(data, pos + 2, value);
}

uint16_t checksum(const Bytes& data)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < data.size(); i += 2)
		sum += (data[i] << 8) | (i + 1 < data.size() ? data[i + 1] : 0);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

uint16_t pppCrc(const Bytes& data)
{
	uint16_t crc = 0xffff;
	for (uint8_t byte : data)
	{
		crc ^= byte;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
	}
	return crc ^ 0xffff;
}

// All I/O runs on the probe's worker. One overall deadline also bounds a peer
// that keeps sending partial frames. The short read timeout permits LCP retries.
class Connection
{
public:
	Connection(const std::string& host, unsigned short port, std::chrono::milliseconds timeout)
		: deadline(Clock::now() + timeout), resolver(io), socket(io), timer(io)
	{
		asio::ip::tcp::resolver::results_type endpoints;
		perform([&](auto done) {
			resolver.async_resolve(asio::ip::tcp::v4(), host, std::to_string(port),
					[&, done](const asio::error_code& ec, auto result) { endpoints = result; done(ec); });
		});
		perform([&](auto done) {
			asio::async_connect(socket, endpoints, [done](const asio::error_code& ec, auto) { done(ec); });
		});
	}

	void send(const Bytes& data)
	{
		perform([&](auto done) {
			asio::async_write(socket, asio::buffer(data),
					[done](const asio::error_code& ec, size_t) { done(ec); });
		});
	}

	uint8_t byte()
	{
		if (position == available)
		{
			// A cancelled read reports zero bytes. Reset both cursors before it can
			// throw, so an LCP retry cannot read beyond the previous receive buffer.
			position = available = 0;
			perform([&](auto done) {
				socket.async_read_some(asio::buffer(buffer),
						[&, done](const asio::error_code& ec, size_t size) { available = size; done(ec); });
			}, std::chrono::seconds(2));
			if (available == 0)
				throw std::runtime_error("Empty service response");
		}
		if (Clock::now() >= deadline)
			throw std::runtime_error("Service deadline exceeded");
		return buffer[position++];
	}

	bool expired() const { return Clock::now() >= deadline; }

private:
	template<typename Start>
	void perform(Start start, std::chrono::milliseconds limit = std::chrono::seconds(15))
	{
		if (expired())
			throw std::runtime_error("Service deadline exceeded");
		io.restart();
		asio::error_code result;
		bool timedOut = false;
		timer.expires_at(std::min(deadline, Clock::now() + limit));
		timer.async_wait([&](const asio::error_code& ec) {
			if (!ec)
			{
				timedOut = true;
				resolver.cancel();
				asio::error_code ignored;
				socket.cancel(ignored);
			}
		});
		start([&](const asio::error_code& ec) { result = ec; timer.cancel(); });
		io.run();
		if (timedOut)
			throw asio::system_error(asio::error::timed_out);
		if (result)
			throw asio::system_error(result);
	}

	Clock::time_point deadline;
	asio::io_context io;
	asio::ip::tcp::resolver resolver;
	asio::ip::tcp::socket socket;
	asio::steady_timer timer;
	std::array<uint8_t, 4096> buffer{};
	size_t position = 0;
	size_t available = 0;
};

// PBA's master is private to DCnet. Use a separate, short-lived PPP connection
// rather than the emulator's global network stack, which may be running a game.
// Routing is resolved through the negotiated VPN DNS, not a public DNS override.
class PbaConnection
{
public:
	PbaConnection(const std::string& host, unsigned short port, std::chrono::milliseconds timeout)
		: connection(host, port, timeout) {}

	void send(uint16_t protocol, const Bytes& payload)
	{
		Bytes raw{0xff, 3, uint8_t(protocol >> 8), uint8_t(protocol)};
		raw.insert(raw.end(), payload.begin(), payload.end());
		const uint16_t crc = pppCrc(raw);
		raw.push_back(crc);
		raw.push_back(crc >> 8);
		Bytes frame{0x7e};
		for (uint8_t byte : raw)
		{
			if (byte < 32 || byte == 0x7d || byte == 0x7e)
			{
				frame.push_back(0x7d);
				byte ^= 0x20;
			}
			frame.push_back(byte);
		}
		frame.push_back(0x7e);
		connection.send(frame);
	}

	void control(uint16_t protocol, uint8_t code, uint8_t id, const Bytes& data = {})
	{
		Bytes packet{code, id, 0, 0};
		put16(packet, 2, data.size() + 4);
		packet.insert(packet.end(), data.begin(), data.end());
		send(protocol, packet);
	}

	uint16_t receive(Bytes& packet)
	{
		for (;;)
		{
			const uint8_t byte = connection.byte();
			if (byte == 0x7e)
			{
				Bytes raw;
				raw.swap(frame);
				escaped = false;
				if (raw.size() < 5)
					continue;
				const uint16_t crc = raw[raw.size() - 2] | (raw.back() << 8);
				raw.resize(raw.size() - 2);
				if (pppCrc(raw) != crc)
					throw std::runtime_error("Invalid PPP checksum");
				size_t pos = raw[0] == 0xff && raw[1] == 3 ? 2 : 0;
				uint16_t protocol = raw.at(pos++);
				if (!(protocol & 1))
					protocol = (protocol << 8) | raw.at(pos++);
				packet.assign(raw.begin() + pos, raw.end());
				return protocol;
			}
			if (byte == 0x7d)
				escaped = true;
			else
			{
				frame.push_back(escaped ? byte ^ 0x20 : byte);
				escaped = false;
				if (frame.size() > 4096)
					throw std::runtime_error("Oversized PPP frame");
			}
		}
	}

	void negotiate()
	{
		control(0xc021, 1, 1);
		bool lcpAck = false, peerAck = false, ipAck = false, ipStarted = false, papSent = false;
		uint16_t auth = 0;
		uint8_t ipId = 0;
		Bytes options{3, 6, 0, 0, 0, 0, 129, 6, 0, 0, 0, 0};
		auto requestIp = [&] {
			ipStarted = true;
			control(0x8021, 1, ++ipId, options);
		};
		for (;;)
		{
			Bytes packet;
			uint16_t protocol;
			try { protocol = receive(packet); }
			catch (const asio::system_error& e) {
				if (e.code() != asio::error::timed_out || connection.expired())
					throw;
				if (!lcpAck)
					control(0xc021, 1, 1);
				else if (ipStarted && !ipAck)
					control(0x8021, 1, ipId, options);
				continue;
			}
			if (protocol != 0xc021 && protocol != 0xc023 && protocol != 0xc223
					&& protocol != 0xc227 && protocol != 0x8021)
				continue;
			if (packet.size() < 4 || be16(packet, 2) != packet.size())
				throw std::runtime_error("Invalid PPP control packet");
			const uint8_t code = packet[0], id = packet[1];
			Bytes data(packet.begin() + 4, packet.end());
			if (protocol == 0xc021)
			{
				if (code == 1)
				{
					for (size_t pos = 0; pos < data.size(); pos += data[pos + 1])
					{
						if (pos + 2 > data.size() || data[pos + 1] < 2 || pos + data[pos + 1] > data.size())
							throw std::runtime_error("Invalid LCP option");
						if (data[pos] == 3)
						{
							if (data[pos + 1] < 4)
								throw std::runtime_error("Invalid PPP authentication option");
							auth = be16(data, pos + 2);
							if (auth != 0xc023 && auth != 0xc227 && !(auth == 0xc223 && data[pos + 1] == 5 && data[pos + 4] == 5))
								throw std::runtime_error("Unsupported PPP authentication");
						}
					}
					control(protocol, 2, id, data);
				}
				else if (code == 2 && id == 1 && data.empty())
					lcpAck = true;
				else if (code == 9)
					echo(id, data);
				else if (code == 5)
					throw std::runtime_error("PPP link terminated");
				if (lcpAck && auth == 0xc023 && !papSent)
				{
					// DCnet's public ISP account, not a player's GameSpy credentials.
					control(0xc023, 1, 1, {8, 'f','l','y','c','a','s','t','1', 8, 'p','a','s','s','w','o','r','d'});
					papSent = true;
				}
			}
			else if (protocol == 0xc227 || protocol == 0xc223)
			{
				if (code == 1)
				{
					if (protocol == 0xc227 && !data.empty() && data[0] == 1)
						control(protocol, 2, id, {1, 'f','l','y','c','a','s','t','1'});
					else
					{
						const size_t pos = protocol == 0xc227 ? 1 : 0;
						if (data.size() <= pos || (pos == 1 && data[0] != 4)
								|| data[pos] == 0 || pos + 1 + data[pos] > data.size())
							throw std::runtime_error("Invalid PPP challenge");
						Bytes input{id, 'p','a','s','s','w','o','r','d'};
						input.insert(input.end(), data.begin() + pos + 1, data.begin() + pos + 1 + data[pos]);
						MD5_CTX context;
						MD5_Init(&context);
						MD5_Update(&context, input.data(), static_cast<unsigned long>(input.size()));
						Bytes response(pos + 17);
						response[0] = pos == 1 ? 4 : 16;
						response[pos] = 16;
						MD5_Final(response.data() + pos + 1, &context);
						response.insert(response.end(), {'f','l','y','c','a','s','t','1'});
						control(protocol, 2, id, response);
					}
				}
				else if (code == 3)
					requestIp();
				else if (code == 4)
					throw std::runtime_error("PPP authentication failed");
			}
			else if (protocol == 0xc023)
			{
				if (code == 2)
					requestIp();
				else if (code == 3)
					throw std::runtime_error("PPP authentication failed");
			}
			else if (protocol == 0x8021)
			{
				for (size_t pos = 0; pos < data.size(); pos += 6)
					if (pos + 6 > data.size() || data[pos + 1] != 6)
						throw std::runtime_error("Invalid IPCP option");
				if (code == 1)
				{
					control(protocol, 2, id, data);
					peerAck = true;
					if (!ipStarted)
						requestIp();
				}
				else if (code == 3 && id == ipId)
				{
					for (size_t pos = 0; pos < data.size(); pos += 6)
						for (size_t own = 0; own < options.size(); own += 6)
							if (data[pos] == options[own])
								std::copy_n(data.begin() + pos + 2, 4, options.begin() + own + 2);
					requestIp();
				}
				else if (code == 2 && id == ipId && data == options)
				{
					ip = be32(data, 2);
					dns = be32(data, 8);
					ipAck = true;
				}
				else if (code == 4)
					throw std::runtime_error("DCnet address negotiation rejected");
				if (peerAck && ipAck && ip != 0 && dns != 0)
					return;
			}
		}
	}

	void sendIp(uint32_t target, uint8_t protocol, Bytes payload)
	{
		Bytes packet(20);
		packet[0] = 0x45;
		put16(packet, 2, packet.size() + payload.size());
		packet[8] = 64;
		packet[9] = protocol;
		put32(packet, 12, ip);
		put32(packet, 16, target);
		put16(packet, 10, checksum(packet));
		packet.insert(packet.end(), payload.begin(), payload.end());
		send(0x21, packet);
	}

	Bytes receiveIp(uint32_t source, uint8_t protocol)
	{
		for (;;)
		{
			Bytes packet;
			const auto type = receive(packet);
			if (type == 0xc021 && packet.size() >= 8 && packet[0] == 9 && be16(packet, 2) == packet.size())
				echo(packet[1], Bytes(packet.begin() + 4, packet.end()));
			if (type != 0x21 || packet.size() < 20 || (packet[0] >> 4) != 4)
				continue;
			const size_t header = (packet[0] & 15) * 4;
			const size_t length = be16(packet, 2);
			if (header < 20 || header > length || length > packet.size() || (be16(packet, 6) & 0x3fff)
					|| packet[9] != protocol || be32(packet, 12) != source || be32(packet, 16) != ip
					|| checksum(Bytes(packet.begin(), packet.begin() + header)) != 0)
				continue;
			return Bytes(packet.begin() + header, packet.begin() + length);
		}
	}

	uint32_t resolveMaster()
	{
		Bytes query{0x44, 0x32, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
			6, 'm','a','s','t','e','r', 7, 'g','a','m','e','s','p','y', 3, 'c','o','m', 0, 0, 1, 0, 1};
		Bytes udp{0xcf, 0x08, 0, 53, 0, 0, 0, 0}; // source port 53000
		put16(udp, 4, query.size() + 8);
		udp.insert(udp.end(), query.begin(), query.end());
		sendIp(dns, 17, udp);
		for (;;)
		{
			Bytes response = receiveIp(dns, 17);
			if (response.size() < 20 || be16(response, 0) != 53 || be16(response, 2) != 53000
					|| be16(response, 4) != response.size() || be16(response, 8) != 0x4432)
				continue;
			response.erase(response.begin(), response.begin() + 8);
			if ((be16(response, 2) & 0x820f) != 0x8000 || be16(response, 4) != 1)
				throw std::runtime_error("DCnet DNS query failed");
			size_t pos = 12;
			auto skipName = [&] {
				for (;;)
				{
					const auto size = response.at(pos++);
					if (size == 0)
						break;
					if ((size & 0xc0) == 0xc0)
					{
						if (pos >= response.size())
							throw std::runtime_error("Truncated DNS pointer");
						++pos;
						break;
					}
					if (size > 63 || pos + size >= response.size())
						throw std::runtime_error("Invalid DNS name");
					pos += size;
				}
			};
			skipName();
			pos += 4;
			const unsigned answers = be16(response, 6);
			for (unsigned i = 0; i < answers; ++i)
			{
				skipName();
				const auto type = be16(response, pos), cls = be16(response, pos + 2), size = be16(response, pos + 8);
				pos += 10;
				if (pos + size > response.size())
					throw std::runtime_error("Truncated DNS answer");
				if (type == 1 && cls == 1 && size == 4 && be32(response, pos) != 0)
					return be32(response, pos);
				pos += size;
			}
			throw std::runtime_error("DCnet master address unavailable");
		}
	}

	// One bounded TCP exchange, transported over the reliable PPP/TCP tunnel.
	// Reject out-of-order data rather than treating a partial banner as success.
	std::string queryLobby(uint32_t target);

private:
	void echo(uint8_t id, Bytes data)
	{
		if (data.size() >= 4)
		{
			std::fill_n(data.begin(), 4, 0);
			control(0xc021, 10, id, data);
		}
	}
	Connection connection;
	Bytes frame;
	bool escaped = false;
	uint32_t ip = 0, dns = 0;
};

std::string pbaRequest(const std::string& greeting)
{
	const size_t pos = greeting.find("\\secure\\");
	if (pos == std::string::npos || pos + 14 > greeting.size())
		throw std::runtime_error("Invalid GameSpy challenge");
	// GameSpy enctype 0, from Luigi Auriemma's GSMSALG (GPL-2.0-or-later).
	// Kbay43 is PBA's public protocol key; it does not identify a player.
	constexpr char key[] = "Kbay43";
	std::array<uint8_t, 256> state;
	unsigned a = 0, b = 0;
	for (unsigned i = 0; i < state.size(); ++i)
		state[i] = i;
	for (unsigned i = 0; i < state.size(); ++i)
	{
		a = (a + state[i] + key[i % 6]) & 255;
		std::swap(state[a], state[i]);
	}
	a = 0;
	std::array<uint8_t, 6> result;
	for (unsigned i = 0; i < result.size(); ++i)
	{
		const uint8_t byte = greeting[pos + 8 + i];
		if (byte < 'A' || byte > 'Z')
			throw std::runtime_error("Invalid GameSpy challenge");
		a = (a + byte + 1) & 255;
		const auto x = state[a];
		b = (b + x) & 255;
		const auto y = state[b];
		std::swap(state[a], state[b]);
		result[i] = byte ^ state[(x + y) & 255];
	}
	constexpr char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string validation;
	for (size_t i = 0; i < result.size(); i += 3)
	{
		const uint32_t bits = (uint32_t(result[i]) << 16) | (result[i + 1] << 8) | result[i + 2];
		for (int shift : {18, 12, 6, 0})
			validation += base64[(bits >> shift) & 63];
	}
	return "\\gamename\\pba2001\\enctype\\0\\validate\\" + validation
			+ "\\final\\\\list\\groups\\gamename\\pba2001\\final\\";
}

std::string PbaConnection::queryLobby(uint32_t target)
{
	uint32_t seq = static_cast<uint32_t>(Clock::now().time_since_epoch().count()), ack = 0;
	bool connected = false, requested = false;
	std::string response;
	auto transmit = [&](uint8_t flags, const std::string& text = std::string()) {
		Bytes tcp(20);
		put16(tcp, 0, 55021);
		put16(tcp, 2, 28900);
		put32(tcp, 4, seq);
		put32(tcp, 8, ack);
		tcp[12] = 0x50;
		tcp[13] = flags;
		put16(tcp, 14, 8192);
		tcp.insert(tcp.end(), text.begin(), text.end());
		Bytes pseudo(12);
		put32(pseudo, 0, ip);
		put32(pseudo, 4, target);
		pseudo[9] = 6;
		put16(pseudo, 10, tcp.size());
		pseudo.insert(pseudo.end(), tcp.begin(), tcp.end());
		put16(tcp, 16, checksum(pseudo));
		sendIp(target, 6, std::move(tcp));
	};
	transmit(2);
	for (;;)
	{
		Bytes tcp = receiveIp(target, 6);
		if (tcp.size() < 20 || be16(tcp, 0) != 28900 || be16(tcp, 2) != 55021)
			continue;
		Bytes pseudo(12);
		put32(pseudo, 0, target);
		put32(pseudo, 4, ip);
		pseudo[9] = 6;
		put16(pseudo, 10, tcp.size());
		pseudo.insert(pseudo.end(), tcp.begin(), tcp.end());
		if (checksum(pseudo) != 0)
			throw std::runtime_error("Invalid PBA TCP checksum");
		const size_t header = (tcp[12] >> 4) * 4;
		if (header < 20 || header > tcp.size() || (tcp[13] & 4))
			throw std::runtime_error("PBA connection rejected");
		if (!connected)
		{
			if ((tcp[13] & 0x12) != 0x12 || be32(tcp, 8) != seq + 1)
				continue;
			++seq;
			ack = be32(tcp, 4) + 1;
			connected = true;
			transmit(16);
		}
		if (tcp.size() > header)
		{
			if (be32(tcp, 4) + ((tcp[13] & 2) ? 1 : 0) != ack
					|| !(tcp[13] & 16) || be32(tcp, 8) != seq)
			{
				transmit(16);
				continue;
			}
			response.append(tcp.begin() + header, tcp.end());
			if (response.size() > 4096)
				throw std::runtime_error("Oversized PBA response");
			ack += tcp.size() - header;
			transmit(16);
			if (!requested && response.find("\\secure\\") != std::string::npos)
			{
				if (response.size() < response.find("\\secure\\") + 14)
					continue;
				const auto request = pbaRequest(response);
				transmit(24, request);
				seq += request.size();
				requested = true;
				response.clear();
			}
			else if (requested && response.find("\\final\\") != std::string::npos)
			{
				transmit(20); // Close without logging in or joining the lobby.
				return response;
			}
		}
		if (tcp[13] & 1)
			throw std::runtime_error("Incomplete PBA response");
	}
}

// C203 framing used by SR2's DCnet raw-modem service. Only request type 566
// (initialization); type 501 proves the service responded without a lobby login.
// Protocol: github.com/flyinghead/sr2-redwango/blob/main/server/c203.py
uint16_t c203Crc(const Bytes& payload)
{
	uint16_t crc = 0;
	for (uint8_t byte : payload)
	{
		crc ^= uint16_t(byte) << 8;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
	}
	return crc;
}

Bytes c203Frame(Bytes payload)
{
	const uint16_t crc = c203Crc(payload);
	payload.at(20) = crc;
	payload.at(21) = crc >> 8;
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
}

bool probePba(const std::string& host, unsigned short port, std::chrono::milliseconds timeout)
{
	try {
		PbaConnection connection(host, port, timeout);
		connection.negotiate();
		const auto response = connection.queryLobby(connection.resolveMaster());
		// This server's lobby counters are constants, so never use them as counts.
		return response.find("\\fieldcount\\7\\groupid\\hostname\\numplayers\\maxwaiting\\numwaiting\\numservers\\password\\") == 0
				&& response.find("\\100\\Lobby\\") != std::string::npos
				&& response.size() >= 7 && response.compare(response.size() - 7, 7, "\\final\\") == 0;
	} catch (const std::exception&) {
		return false;
	}
}

bool probeSegaRally(const std::string& host, unsigned short port, std::chrono::milliseconds timeout)
{
	try {
		Connection connection(host, port, timeout);
		Bytes hello(26);
		hello[24] = 566 & 255;
		hello[25] = 566 >> 8;
		connection.send(c203Frame(hello));
		Bytes packet;
		bool marker = false, started = false;
		for (;;)
		{
			const uint8_t byte = connection.byte();
			if (marker)
			{
				marker = false;
				if (byte == 0x60 && !started)
					started = true;
				else if (byte == 0x61 && started)
				{
					if (packet.size() != 138 || packet[24] != (501 & 255) || packet[25] != (501 >> 8)
							|| packet[22] != 0 || packet[23] != 0)
						return false;
					const uint16_t expected = packet[20] | (packet[21] << 8);
					packet[20] = packet[21] = 0;
					return c203Crc(packet) == expected;
				}
				else if (byte == 0x70 && started)
					packet.push_back(byte);
				else
					return false;
			}
			else if (byte == 0x70)
				marker = true;
			else if (started)
				packet.push_back(byte);
			if (packet.size() > 4096)
				return false;
		}
	} catch (const std::exception&) {
		return false;
	}
}
}
