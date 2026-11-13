/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
// Pure, dependency-light helpers for Remote Play: WebSocket handshake and
// framing (RFC 6455), PIN comparison, player slots and input messages.
// Kept free of emulator state so unit tests can exercise them directly.
#include "types.h"
#include "input/gamepad.h"
#include "remote_play.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace remoteplay {

// --- SHA-1 (FIPS 180-1), needed for the WebSocket accept key ---
class Sha1
{
public:
	Sha1() { reset(); }
	void reset()
	{
		h = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
		totalLen = 0;
		bufLen = 0;
	}
	void update(const u8 *data, size_t len)
	{
		totalLen += len;
		while (len > 0)
		{
			size_t take = std::min(len, sizeof(buf) - bufLen);
			std::memcpy(buf + bufLen, data, take);
			bufLen += take;
			data += take;
			len -= take;
			if (bufLen == sizeof(buf))
			{
				process(buf);
				bufLen = 0;
			}
		}
	}
	std::array<u8, 20> finish()
	{
		u64 bitLen = totalLen * 8;
		u8 pad = 0x80;
		update(&pad, 1);
		pad = 0;
		while (bufLen != 56)
			update(&pad, 1);
		u8 lenBytes[8];
		for (int i = 0; i < 8; i++)
			lenBytes[i] = (u8)(bitLen >> (56 - 8 * i));
		// Feed the length through the block buffer without counting it.
		std::memcpy(buf + bufLen, lenBytes, 8);
		process(buf);
		bufLen = 0;
		std::array<u8, 20> out;
		for (int i = 0; i < 5; i++)
		{
			out[i * 4] = (u8)(h[i] >> 24);
			out[i * 4 + 1] = (u8)(h[i] >> 16);
			out[i * 4 + 2] = (u8)(h[i] >> 8);
			out[i * 4 + 3] = (u8)h[i];
		}
		return out;
	}

private:
	static u32 rol(u32 v, int bits) { return (v << bits) | (v >> (32 - bits)); }
	void process(const u8 *block)
	{
		u32 w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (u32)block[i * 4] << 24 | (u32)block[i * 4 + 1] << 16 | (u32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++)
		{
			u32 f, k;
			if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
			else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
			else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
			u32 tmp = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(b, 30); b = a; a = tmp;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	std::array<u32, 5> h;
	u64 totalLen = 0;
	u8 buf[64] = {};
	size_t bufLen = 0;
};

inline std::string base64Encode(const u8 *data, size_t len)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	for (size_t i = 0; i < len; i += 3)
	{
		u32 v = (u32)data[i] << 16;
		if (i + 1 < len) v |= (u32)data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		out += table[(v >> 18) & 63];
		out += table[(v >> 12) & 63];
		out += i + 1 < len ? table[(v >> 6) & 63] : '=';
		out += i + 2 < len ? table[v & 63] : '=';
	}
	return out;
}

// RFC 6455 section 1.3: Sec-WebSocket-Accept for a client Sec-WebSocket-Key.
inline std::string webSocketAcceptKey(const std::string& clientKey)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	Sha1 sha;
	sha.update((const u8 *)clientKey.data(), clientKey.size());
	sha.update((const u8 *)guid, sizeof(guid) - 1);
	auto digest = sha.finish();
	return base64Encode(digest.data(), digest.size());
}

// --- WebSocket framing (RFC 6455) ---
enum class WsOpcode : u8 { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// Encode one server-to-client frame (never masked).
inline std::string wsEncodeFrame(WsOpcode opcode, const std::string& payload)
{
	std::string out;
	out += (char)(0x80 | (u8)opcode);
	if (payload.size() < 126)
		out += (char)payload.size();
	else if (payload.size() <= 0xFFFF)
	{
		out += (char)126;
		out += (char)(payload.size() >> 8);
		out += (char)payload.size();
	}
	else
	{
		out += (char)127;
		for (int i = 7; i >= 0; i--)
			out += (char)(payload.size() >> (8 * i));
	}
	out += payload;
	return out;
}

struct WsFrame
{
	WsOpcode opcode = WsOpcode::Text;
	bool fin = true;
	std::string payload;
};

// Incremental parser for client-to-server frames (must be masked).
// Feed bytes; returns bytes consumed (0 = need more), or -1 on protocol error.
// Fragmented messages must be reassembled by the caller.
inline int wsParseFrame(const u8 *data, size_t len, WsFrame& out)
{
	if (len < 2)
		return 0;
	out.fin = (data[0] & 0x80) != 0;
	out.opcode = (WsOpcode)(data[0] & 0x0F);
	bool masked = (data[1] & 0x80) != 0;
	u64 payloadLen = data[1] & 0x7F;
	size_t headerLen = 2;
	if (payloadLen == 126)
	{
		if (len < headerLen + 2) return 0;
		payloadLen = (u64)data[2] << 8 | data[3];
		headerLen += 2;
	}
	else if (payloadLen == 127)
	{
		if (len < headerLen + 8) return 0;
		payloadLen = 0;
		for (int i = 0; i < 8; i++)
			payloadLen = payloadLen << 8 | data[2 + i];
		headerLen += 8;
	}
	if (!masked || payloadLen > (16ull << 20))
		return -1;
	if (len < headerLen + 4 + payloadLen)
		return 0;
	const u8 *mask = data + headerLen;
	const u8 *body = mask + 4;
	out.payload.resize((size_t)payloadLen);
	for (size_t i = 0; i < payloadLen; i++)
		out.payload[i] = (char)(body[i] ^ mask[i % 4]);
	return (int)(headerLen + 4 + payloadLen);
}

// Constant-time PIN comparison so response timing does not leak the PIN.
inline bool pinMatches(const std::string& candidate, const std::string& expected)
{
	if (expected.empty())
		return false;
	u8 diff = candidate.size() == expected.size() ? 0 : 1;
	size_t n = std::max(candidate.size(), expected.size());
	for (size_t i = 0; i < n; i++)
	{
		u8 a = i < candidate.size() ? (u8)candidate[i] : 0;
		u8 b = i < expected.size() ? (u8)expected[i] : 0;
		diff |= a ^ b;
	}
	return diff == 0;
}

// Player slots 0-3 (Dreamcast maple ports). Thread-safe claim/release.
class PlayerSlots
{
public:
	// requested < 0 picks the first free slot. Returns the slot or -1.
	int claim(int requested)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (requested >= 0)
		{
			if (requested > 3 || states[requested] != PlayerState::Available)
				return -1;
			states[requested] = PlayerState::Joining;
			return requested;
		}
		for (int i = 0; i < 4; i++)
			if (states[i] == PlayerState::Available)
			{
				states[i] = PlayerState::Joining;
				return i;
			}
		return -1;
	}
	// A PIN login reserves a slot; only an attached WebSocket is connected.
	bool connect(int slot)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (slot < 0 || slot >= 4 || states[slot] != PlayerState::Joining)
			return false;
		states[slot] = PlayerState::Connected;
		return true;
	}
	void release(int slot)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (slot >= 0 && slot < 4)
			states[slot] = PlayerState::Available;
	}
	bool isUsed(int slot) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return slot >= 0 && slot < 4 && states[slot] != PlayerState::Available;
	}
	int usedCount() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		int n = 0;
		for (PlayerState state : states) n += state != PlayerState::Available ? 1 : 0;
		return n;
	}
	std::array<PlayerState, 4> snapshot() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return states;
	}
	void reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		states = {};
	}

private:
	mutable std::mutex mutex;
	std::array<PlayerState, 4> states = {};
};

// --- Phone input messages ---
// Names sent by the web client for buttons and analog triggers.
inline u32 remoteControlFromName(const std::string& name)
{
	if (name == "A") return DC_BTN_A;
	if (name == "B") return DC_BTN_B;
	if (name == "X") return DC_BTN_X;
	if (name == "Y") return DC_BTN_Y;
	if (name == "START") return DC_BTN_START;
	if (name == "UP") return DC_DPAD_UP;
	if (name == "DOWN") return DC_DPAD_DOWN;
	if (name == "LEFT") return DC_DPAD_LEFT;
	if (name == "RIGHT") return DC_DPAD_RIGHT;
	if (name == "LT") return DC_AXIS_LT;
	if (name == "RT") return DC_AXIS_RT;
	if (name == "MENU") return EMU_BTN_MENU;
	return 0;
}

struct RemoteInputEvent
{
	enum class Type { Button, Trigger, Stick, ReleaseAll, Ping };
	Type type = Type::Ping;
	u32 key = 0;		// DreamcastKey for Button/Trigger
	bool pressed = false;
	float value = 0;	// trigger 0..1
	float x = 0, y = 0;	// stick -1..1
};

namespace detail {
inline bool jsonString(const std::string& s, const char *key, std::string& out)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t p = s.find(pat);
	if (p == std::string::npos) return false;
	p = s.find(':', p + pat.size());
	if (p == std::string::npos) return false;
	p = s.find('"', p);
	if (p == std::string::npos) return false;
	size_t end = s.find('"', p + 1);
	if (end == std::string::npos) return false;
	out = s.substr(p + 1, end - p - 1);
	return true;
}
inline bool jsonNumber(const std::string& s, const char *key, double& out)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t p = s.find(pat);
	if (p == std::string::npos) return false;
	p = s.find(':', p + pat.size());
	if (p == std::string::npos) return false;
	p++;
	while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
	try {
		size_t used = 0;
		out = std::stod(s.substr(p), &used);
		return used > 0;
	} catch (...) { return false; }
}
} // namespace detail

// Parse one input message. Returns false on malformed/unknown input;
// callers must drop such messages without touching gamepad state.
inline bool parseRemoteInputMessage(const std::string& msg, RemoteInputEvent& out)
{
	if (msg.empty() || msg.size() > 512)
		return false;
	std::string type;
	if (!detail::jsonString(msg, "t", type))
		return false;
	if (type == "ping")
	{
		out = RemoteInputEvent{};
		out.type = RemoteInputEvent::Type::Ping;
		return true;
	}
	if (type == "release")
	{
		out = RemoteInputEvent{};
		out.type = RemoteInputEvent::Type::ReleaseAll;
		return true;
	}
	if (type == "stick")
	{
		double x = 0, y = 0;
		if (!detail::jsonNumber(msg, "x", x) || !detail::jsonNumber(msg, "y", y))
			return false;
		out = RemoteInputEvent{};
		out.type = RemoteInputEvent::Type::Stick;
		out.x = (float)std::max(-1.0, std::min(1.0, x));
		out.y = (float)std::max(-1.0, std::min(1.0, y));
		return true;
	}
	if (type == "btn" || type == "axis")
	{
		std::string key;
		if (!detail::jsonString(msg, "k", key))
			return false;
		u32 code = remoteControlFromName(key);
		if (code == 0)
			return false;
		out = RemoteInputEvent{};
		out.key = code;
		if (type == "btn")
		{
			if (code == DC_AXIS_LT || code == DC_AXIS_RT)
				return false;
			double p = 0;
			if (!detail::jsonNumber(msg, "p", p))
				return false;
			out.type = RemoteInputEvent::Type::Button;
			out.pressed = p != 0;
			return true;
		}
		if (code != DC_AXIS_LT && code != DC_AXIS_RT)
			return false;
		double v = 0;
		if (!detail::jsonNumber(msg, "v", v))
			return false;
		out.type = RemoteInputEvent::Type::Trigger;
		out.value = (float)std::max(0.0, std::min(1.0, v));
		return true;
	}
	return false;
}

} // namespace remoteplay
