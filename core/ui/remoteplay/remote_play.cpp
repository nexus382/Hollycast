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
// Remote Play server: one listening socket serving the phone page (HTTP),
// an MJPEG video stream and phone controller input (WebSocket, RFC 6455).
// Capture stays on the render thread; encoding runs on the capture worker.
// The server binds localhost by default; a Cloudflare quick tunnel (or an
// explicit LAN bind) is how phones reach it. A session PIN gates login.
#include "remote_play.h"
#include "remote_play_page.h"
#include "remote_play_protocol.h"
#include "cloudflare_tunnel.h"
#include "cfg/option.h"
#include "h264_encoder.h"
#include "webrtc_peer.h"
#include "audio_stream.h"
#include "hw/maple/maple_if.h"
#include "hw/pvr/Renderer_if.h"
#include "input/remote_gamepad.h"
#include "log/Log.h"
#include "network/net_platform.h"
#include "ui/gui.h"
#include "ui/settings.h"

#include <stb_image_write.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

namespace remoteplay {
namespace {

std::atomic<bool> running { false };
sock_t listenSocket = INVALID_SOCKET;
std::thread acceptThread;
std::thread captureThread;

std::mutex stateMutex;
std::string sessionPin;
std::string startError;
std::unordered_map<std::string, int> sessions;	// token -> player slot
PlayerSlots slots;
std::array<std::shared_ptr<RemoteGamepad>, 4> pads;

struct LoginGuard { int failures = 0; std::chrono::steady_clock::time_point blockedUntil {}; };
std::mutex loginMutex;
std::unordered_map<std::string, LoginGuard> loginGuards;

std::mutex clientsMutex;
std::set<sock_t> clientSockets;
std::atomic<int> streamViewers { 0 };

std::mutex frameMutex;
std::condition_variable frameCv;
std::string latestJpeg;
u64 latestSeq = 0;

struct CaptureRequest
{
	std::vector<u8> rgb;
	int width = 0;
	int height = 0;
	std::promise<bool> result;
};
std::mutex captureMutex;
std::shared_ptr<CaptureRequest> pendingCapture;

bool sendAll(sock_t sock, const char *data, size_t len)
{
	while (len > 0)
	{
		int sent = (int)send(sock, data, (int)std::min<size_t>(len, 1 << 20), 0);
		if (sent <= 0)
			return false;
		data += sent;
		len -= sent;
	}
	return true;
}
bool sendAll(sock_t sock, const std::string& s) { return sendAll(sock, s.data(), s.size()); }

void setNoDelay(sock_t sock)
{
	int on = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
}

std::string makeToken()
{
	std::random_device rd;
	std::string out;
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++)
		out += hex[rd() & 0xF];
	return out;
}

std::string makePin()
{
	std::random_device rd;
	char buf[8];
	snprintf(buf, sizeof(buf), "%06u", (unsigned)(rd() % 1000000));
	return buf;
}

struct HttpRequest
{
	std::string method;
	std::string path;	// without query
	std::string query;
	std::map<std::string, std::string> headers;	// lower-case keys
	std::string body;
	std::string clientIp;
};

std::string lowerCase(std::string s)
{
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

std::string headerValue(const HttpRequest& req, const char *name)
{
	auto it = req.headers.find(name);
	return it == req.headers.end() ? "" : it->second;
}

// Read one HTTP request (headers + Content-Length body). False on close/error.
bool readRequest(sock_t sock, HttpRequest& req)
{
	std::string data;
	char buf[8192];
	while (data.find("\r\n\r\n") == std::string::npos)
	{
		int n = (int)recv(sock, buf, sizeof(buf), 0);
		if (n <= 0)
			return false;
		data.append(buf, n);
		if (data.size() > 65536)
			return false;
	}
	size_t headEnd = data.find("\r\n\r\n");
	std::string head = data.substr(0, headEnd);
	req.body = data.substr(headEnd + 4);
	size_t lineEnd = head.find("\r\n");
	std::string line = head.substr(0, lineEnd);
	size_t sp1 = line.find(' ');
	size_t sp2 = line.rfind(' ');
	if (sp1 == std::string::npos || sp2 == sp1)
		return false;
	req.method = line.substr(0, sp1);
	std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t q = target.find('?');
	req.path = q == std::string::npos ? target : target.substr(0, q);
	req.query = q == std::string::npos ? "" : target.substr(q + 1);
	size_t pos = lineEnd + 2;
	while (pos < head.size())
	{
		size_t eol = head.find("\r\n", pos);
		std::string hline = head.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
		size_t colon = hline.find(':');
		if (colon != std::string::npos)
		{
			std::string value = hline.substr(colon + 1);
			size_t first = value.find_first_not_of(" \t");
			req.headers[lowerCase(hline.substr(0, colon))] = first == std::string::npos ? "" : value.substr(first);
		}
		if (eol == std::string::npos)
			break;
		pos = eol + 2;
	}
	int contentLength = 0;
	try { contentLength = std::stoi(headerValue(req, "content-length")); } catch (...) {}
	while ((int)req.body.size() < contentLength)
	{
		int n = (int)recv(sock, buf, (int)std::min<size_t>(sizeof(buf), contentLength - req.body.size()), 0);
		if (n <= 0)
			return false;
		req.body.append(buf, n);
	}
	if ((int)req.body.size() > contentLength)
		req.body.resize(contentLength);
	return true;
}

void sendResponse(sock_t sock, int code, const char *status, const std::string& contentType,
		const std::string& body, const std::string& extraHeaders = "")
{
	std::string head = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n"
		"Content-Type: " + contentType + "\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n"
		"Cache-Control: no-store\r\n"
		"X-Content-Type-Options: nosniff\r\n" + extraHeaders + "Connection: close\r\n\r\n";
	sendAll(sock, head);
	if (!body.empty())
		sendAll(sock, body);
}

int sessionPlayer(const HttpRequest& req)
{
	std::string cookie = headerValue(req, "cookie");
	size_t p = cookie.find("rp_session=");
	if (p == std::string::npos)
		return -1;
	std::string token = cookie.substr(p + 11);
	size_t end = token.find_first_of("; \t");
	token = token.substr(0, end);
	std::lock_guard<std::mutex> lock(stateMutex);
	auto it = sessions.find(token);
	return it == sessions.end() ? -1 : it->second;
}

void handleLogin(sock_t sock, const HttpRequest& req)
{
	{
		std::lock_guard<std::mutex> lock(loginMutex);
		LoginGuard& guard = loginGuards[req.clientIp];
		if (guard.blockedUntil > std::chrono::steady_clock::now())
		{
			sendResponse(sock, 429, "Too Many Requests", "application/json", "{\"error\":\"Too many failed attempts. Wait a minute and try again.\"}");
			return;
		}
	}
	std::string pin;
	detail::jsonString(req.body, "pin", pin);
	int requested = -1;
	double playerNum = -1;
	if (detail::jsonNumber(req.body, "player", playerNum))
		requested = (int)playerNum;
	std::string expected;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		expected = sessionPin;
	}
	if (!pinMatches(pin, expected))
	{
		std::lock_guard<std::mutex> lock(loginMutex);
		LoginGuard& guard = loginGuards[req.clientIp];
		if (++guard.failures >= 5)
		{
			guard.blockedUntil = std::chrono::steady_clock::now() + std::chrono::minutes(1);
			guard.failures = 0;
		}
		sendResponse(sock, 403, "Forbidden", "application/json", "{\"error\":\"Wrong PIN.\"}");
		return;
	}
	int slot = slots.claim(requested);
	if (slot < 0)
	{
		sendResponse(sock, 409, "Conflict", "application/json",
			requested >= 0 ? "{\"error\":\"That controller is already in use. Pick another.\"}"
						   : "{\"error\":\"All four controller slots are in use.\"}");
		return;
	}
	std::string token = makeToken();
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		sessions[token] = slot;
	}
	{
		std::lock_guard<std::mutex> lock(loginMutex);
		loginGuards.erase(req.clientIp);
	}
	std::string cookie = "Set-Cookie: rp_session=" + token + "; HttpOnly; SameSite=Strict; Path=/\r\n";
	sendResponse(sock, 200, "OK", "application/json", "{\"player\":" + std::to_string(slot) + "}", cookie);
}

void handleStream(sock_t sock)
{
	setNoDelay(sock);
	streamViewers++;
	std::string head = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n";
	bool ok = sendAll(sock, head);
	u64 lastSent = 0;
	while (ok && running)
	{
		std::string jpeg;
		u64 seq = 0;
		{
			std::unique_lock<std::mutex> lock(frameMutex);
			if (!frameCv.wait_for(lock, std::chrono::seconds(5), [&] { return latestSeq != lastSent || !running; }))
				continue;
			if (!running)
				break;
			jpeg = latestJpeg;
			seq = latestSeq;
		}
		if (jpeg.empty() || seq == lastSent)
			continue;
		lastSent = seq;
		std::string part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
			+ std::to_string(jpeg.size()) + "\r\n\r\n";
		ok = sendAll(sock, part) && sendAll(sock, jpeg) && sendAll(sock, "\r\n");
	}
	streamViewers--;
}

// A phone can only be seen by a game if a controller device exists on its
// maple port. Ports 2-4 default to "None", so create a Sega Controller there
// when a phone connects (same live-reconnect the Settings page performs).
void ensureControllerDevice(int slot)
{
	gui_runOnUiThread([slot] {
		if (config::MapleMainDevices[slot] == MDT_None)
		{
			config::MapleMainDevices[slot] = MDT_SegaController;
			if (game_started && settings.platform.isConsole())
			{
				maple_ReconnectDevices();
				reset_vmus();
			}
		}
	});
}

void releasePlayerSession(int player)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	for (auto it = sessions.begin(); it != sessions.end();)
	{
		if (it->second == player)
			it = sessions.erase(it);
		else
			++it;
	}
	slots.release(player);
}

void handleWebSocket(sock_t sock, const HttpRequest& req, int player)
{
	std::string key = headerValue(req, "sec-websocket-key");
	if (key.empty())
	{
		sendResponse(sock, 400, "Bad Request", "text/plain", "Missing Sec-WebSocket-Key");
		return;
	}
	if (!slots.connect(player))
	{
		sendResponse(sock, 409, "Conflict", "text/plain", "Controller already connected");
		return;
	}
	std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: " + webSocketAcceptKey(key) + "\r\n\r\n";
	if (!sendAll(sock, handshake))
	{
		releasePlayerSession(player);
		return;
	}
	setNoDelay(sock);
	ensureControllerDevice(player);
	auto pad = pads[player];
	// WebRTC signaling sends happen on libdatachannel threads while pongs
	// are sent here: serialize socket writes through one mutex.
	auto sendMutex = std::make_shared<std::mutex>();
	auto alive = std::make_shared<std::atomic<bool>>(true);
	auto inputMutex = std::make_shared<std::mutex>();
	auto applyInput = [pad, alive, inputMutex](const std::string& message) {
		std::lock_guard<std::mutex> lock(*inputMutex);
		RemoteInputEvent event;
		if (*alive && pad && parseRemoteInputMessage(message, event)) pad->applyEvent(event);
	};
	auto sendText = [sock, sendMutex, alive](const std::string& text) {
		if (!*alive)
			return;
		std::lock_guard<std::mutex> lock(*sendMutex);
		sendAll(sock, wsEncodeFrame(WsOpcode::Text, text));
	};
	if (webrtcAvailable())
	{
		sendText("{\"t\":\"rtc-ready\"}");
		rtcSessionStart(player, sendText, applyInput);
	}
	std::string buffer;
	std::string message;
	char buf[4096];
	while (running)
	{
		int n = (int)recv(sock, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		buffer.append(buf, n);
		bool dead = false;
		while (!buffer.empty())
		{
			WsFrame frame;
			int used = wsParseFrame((const u8 *)buffer.data(), buffer.size(), frame);
			if (used == 0)
				break;
			if (used < 0) { dead = true; break; }
			buffer.erase(0, used);
			if (frame.opcode == WsOpcode::Close) { dead = true; break; }
			if (frame.opcode == WsOpcode::Ping)
			{
				std::lock_guard<std::mutex> lock(*sendMutex);
				if (!sendAll(sock, wsEncodeFrame(WsOpcode::Pong, frame.payload))) { dead = true; break; }
				continue;
			}
			if (frame.opcode != WsOpcode::Text && frame.opcode != WsOpcode::Continuation)
				continue;
			message += frame.payload;
			if (!frame.fin)
				continue;
			if (message.find("\"rtc\"") != std::string::npos && message.find("\"kind\"") != std::string::npos)
			{
				rtcSessionHandleMessage(player, message);
				message.clear();
				continue;
			}
			RemoteInputEvent event;
			if (parseRemoteInputMessage(message, event))
			{
				if (event.type == RemoteInputEvent::Type::Ping)
					sendText("{\"t\":\"pong\"}");
				else if (pad != nullptr)
					applyInput(message);
			}
			message.clear();
			if (message.size() > 4096)
				message.clear();
		}
		if (dead)
			break;
	}
	*alive = false;
	rtcSessionStop(player);
	if (pad != nullptr)
	{
		std::lock_guard<std::mutex> lock(*inputMutex);
		pad->releaseAll();
	}
	// The phone is gone: free its controller slot and end its sessions.
	releasePlayerSession(player);
}

void appendJpeg(void *context, void *data, int size)
{
	auto *out = (std::string *)context;
	out->append((const char *)data, size);
}

// Grab the last rendered frame on the UI thread (GPU readback must happen
// there) as RGB24. False when the renderer has no frame yet (e.g. while a
// game loads); callers then publish nothing rather than a black frame,
// which would make the phone stream flash between real frames.
bool captureRgb(std::vector<u8>& rgb, int& w, int& h)
{
	int width = std::max(160, std::min(1280, (int)config::RemotePlayWidth));
	auto state = std::make_shared<CaptureRequest>();
	state->width = width;
	auto future = state->result.get_future();
	{
		std::lock_guard<std::mutex> lock(captureMutex);
		pendingCapture = state;
	}
	bool got = false;
	if (future.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready)
		got = future.get();
	else
	{
		std::lock_guard<std::mutex> lock(captureMutex);
		if (pendingCapture == state)
			pendingCapture.reset();
	}
	if (!got || state->width <= 0 || state->height <= 0
			|| state->rgb.size() < (size_t)state->width * state->height * 3)
		return false;
	rgb = std::move(state->rgb);
	w = state->width;
	h = state->height;
	return true;
}

bool encodeJpeg(const std::vector<u8>& rgb, int w, int h, std::string& out)
{
	int quality = std::max(30, std::min(95, (int)config::RemotePlayJpegQuality));
	out.clear();
	return stbi_write_jpg_to_func(appendJpeg, &out, w, h, 3, rgb.data(), quality) != 0;
}

std::string blackJpeg(int width)
{
	int bh = width * 9 / 16;
	std::vector<u8> pixels((size_t)width * bh * 3, 0);
	std::string out;
	stbi_write_jpg_to_func(appendJpeg, &out, width, bh, 3, pixels.data(), 70);
	return out;
}

void captureLoop()
{
	bool haveRealFrame = false;
	std::unique_ptr<H264Encoder> encoder;
	while (running)
	{
		bool wantJpeg = streamViewers > 0;
		bool wantRtc = rtcAnySession();
		if (!wantJpeg && !wantRtc)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		auto frameStart = std::chrono::steady_clock::now();
		int fps = std::max(5, std::min(60, (int)config::RemotePlayFps));
		std::vector<u8> rgb;
		int w = 0, h = 0;
		bool gotFrame = captureRgb(rgb, w, h);
		if (gotFrame)
			haveRealFrame = true;
		else if (!haveRealFrame && wantJpeg)
		{
			// Black placeholder only until the first real frame arrives.
			std::string jpeg = blackJpeg(std::max(160, std::min(1280, (int)config::RemotePlayWidth)));
			if (!jpeg.empty())
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				latestJpeg = std::move(jpeg);
				latestSeq++;
			}
			frameCv.notify_all();
		}
		if (gotFrame && wantJpeg)
		{
			std::string jpeg;
			if (encodeJpeg(rgb, w, h, jpeg) && !jpeg.empty())
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				latestJpeg = std::move(jpeg);
				latestSeq++;
			}
			frameCv.notify_all();
		}
		if (gotFrame && wantRtc && h264EncoderAvailable())
		{
			if (encoder == nullptr)
			{
				// Timestamp the actual output cadence. Counting nominal FPS
				// ticks makes the receiver's clock drift when the game drops
				// frames, building delay precisely when the host is busy.
				auto epoch = std::chrono::steady_clock::now();
				encoder = H264Encoder::create(w, h, fps,
					std::max(200, std::min(8000, (int)config::RemotePlayBitrateKbps)),
					[epoch](const std::vector<u8>& sample, bool keyframe) {
						auto elapsed = std::chrono::steady_clock::now() - epoch;
						uint32_t timestamp = (uint32_t)(std::chrono::duration<double>(elapsed).count() * 90000.0);
						rtcPushSample(sample, keyframe, timestamp);
					});
			}
			if (encoder != nullptr)
			{
				if (rtcKeyframeRequested())
				{
					encoder->requestKeyframe();
					rtcClearKeyframeRequest();
				}
				encoder->encodeRgb(rgb.data(), w, h);
			}
		}
		auto elapsed = std::chrono::steady_clock::now() - frameStart;
		auto target = std::chrono::milliseconds(1000 / fps);
		if (elapsed < target)
			std::this_thread::sleep_for(target - elapsed);
	}
}

void handleClient(sock_t sock, std::string clientIp)
{
	{
		std::lock_guard<std::mutex> lock(clientsMutex);
		clientSockets.insert(sock);
	}
	HttpRequest req;
	req.clientIp = clientIp;
	if (readRequest(sock, req))
	{
		int player = sessionPlayer(req);
		if (req.method == "GET" && req.path == "/")
			sendResponse(sock, 200, "OK", "text/html; charset=utf-8", remotePlayPage());
		else if (req.method == "POST" && req.path == "/api/login")
			handleLogin(sock, req);
		else if (req.method == "GET" && req.path == "/api/status")
		{
			if (player < 0)
				sendResponse(sock, 401, "Unauthorized", "application/json", "{\"error\":\"Not logged in.\"}");
			else
				sendResponse(sock, 200, "OK", "application/json",
					"{\"player\":" + std::to_string(player) + ",\"players\":" + std::to_string(slots.usedCount()) + "}");
		}
		else if (req.method == "GET" && req.path == "/stream.mjpg")
		{
			if (player < 0)
				sendResponse(sock, 401, "Unauthorized", "text/plain", "Log in first.");
			else
				handleStream(sock);
		}
		else if (req.method == "GET" && req.path == "/ws")
		{
			if (player < 0)
				sendResponse(sock, 401, "Unauthorized", "text/plain", "Log in first.");
			else
				handleWebSocket(sock, req, player);
		}
		else
			sendResponse(sock, 404, "Not Found", "text/plain", "Not found.");
	}
	{
		std::lock_guard<std::mutex> lock(clientsMutex);
		clientSockets.erase(sock);
	}
	closesocket(sock);
}

void acceptLoop()
{
	while (running)
	{
		sockaddr_in addr {};
		socklen_t addrLen = sizeof(addr);
		sock_t client = accept(listenSocket, (sockaddr *)&addr, &addrLen);
#ifndef _WIN32
		if (VALID(client))
			fcntl(client, F_SETFD, FD_CLOEXEC);
#endif
		if (!VALID(client))
		{
			if (running)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		std::thread(handleClient, client, std::string(ip)).detach();
	}
}
} // namespace

void capturePresentedFrame()
{
	std::shared_ptr<CaptureRequest> request;
	{
		std::lock_guard<std::mutex> lock(captureMutex);
		request.swap(pendingCapture);
	}
	if (request == nullptr)
		return;
	bool captured = false;
	try {
		captured = running && renderer != nullptr
			&& renderer->GetLastFrameForRemotePlay(request->rgb, request->width, request->height);
	} catch (const std::exception& e) {
		WARN_LOG(RENDERER, "Remote Play capture failed: %s", e.what());
	}
	request->result.set_value(captured);
}

bool serverStart(std::string& error)
{
	if (running)
		return true;
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	int port = std::max(1, std::min(65535, (int)config::RemotePlayPort));
	listenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (!VALID(listenSocket))
	{
		error = "Could not create a network socket.";
		std::lock_guard<std::mutex> lock(stateMutex);
		startError = error;
		return false;
	}
#ifndef _WIN32
	// Children (cloudflared) must not inherit this socket across fork/exec:
	// an orphaned tunnel process would keep the port bound after we exit,
	// and the next Hollycast could never start Remote Play again.
	fcntl(listenSocket, F_SETFD, FD_CLOEXEC);
#endif
	int reuse = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16)port);
	addr.sin_addr.s_addr = config::RemotePlayBindLan ? INADDR_ANY : htonl(INADDR_LOOPBACK);
	if (bind(listenSocket, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(listenSocket, 16) != 0)
	{
		error = "Could not listen on port " + std::to_string(port) + ". Is another program using it?";
		closesocket(listenSocket);
		listenSocket = INVALID_SOCKET;
		std::lock_guard<std::mutex> lock(stateMutex);
		startError = error;
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		std::string configuredPin = (std::string)config::RemotePlayPin;
		sessionPin = configuredPin.empty() ? makePin() : configuredPin;
		sessions.clear();
		startError.clear();
	}
	slots.reset();
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		latestJpeg.clear();
		latestSeq = 0;
	}
	for (int i = 0; i < 4; i++)
	{
		pads[i] = std::make_shared<RemoteGamepad>(i);
		GamepadDevice::Register(pads[i]);
	}
	running = true;
	startAudioStreaming();
	acceptThread = std::thread(acceptLoop);
	captureThread = std::thread(captureLoop);
	return true;
}

void serverStop()
{
	if (!running && !VALID(listenSocket))
		return;
	running = false;
	stopAudioStreaming();
	frameCv.notify_all();
	if (VALID(listenSocket))
	{
		closesocket(listenSocket);
		listenSocket = INVALID_SOCKET;
	}
	{
		std::lock_guard<std::mutex> lock(clientsMutex);
		for (sock_t client : clientSockets)
			closesocket(client);
	}
	if (acceptThread.joinable())
		acceptThread.join();
	if (captureThread.joinable())
		captureThread.join();
	for (int i = 0; i < 4; i++)
	{
		if (pads[i] != nullptr)
		{
			pads[i]->releaseAll();
			GamepadDevice::Unregister(pads[i]);
			pads[i].reset();
		}
	}
	slots.reset();
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		sessions.clear();
	}
	tunnelStop();
}

bool serverIsRunning() { return running; }

ServerStatus serverStatus()
{
	ServerStatus out;
	out.running = running;
	out.port = (int)config::RemotePlayPort;
	out.localUrl = "http://127.0.0.1:" + std::to_string(out.port);
	out.streamViewers = streamViewers;
	out.players = slots.snapshot();
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (running)
			out.pin = sessionPin;
		out.error = startError;
	}
	return out;
}

void autoStartIfEnabled()
{
	if (!config::RemotePlayAutoStart)
		return;
	std::string error;
	if (!serverStart(error))
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		startError = error;
		return;
	}
	if (config::RemotePlayAutoTunnel)
	{
		std::string tunnelError;
		tunnelStart((int)config::RemotePlayPort, tunnelError);
	}
}

} // namespace remoteplay
