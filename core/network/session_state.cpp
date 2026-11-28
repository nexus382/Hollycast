// Portions Copyright 2026 The Hollycast Authors
#include "session_state.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#ifndef _WIN32
#include <poll.h>
#endif

namespace sessionstate
{
namespace
{

constexpr size_t MAX_ITEMS = 16;
constexpr size_t MAX_ITEM_SIZE = 256 * 1024;
constexpr size_t MAX_TOTAL_SIZE = 2 * 1024 * 1024;
constexpr u16 FORMAT_VERSION = 1;
// The zero prefix includes an invalid GGPO message type so late echoes are ignored.
constexpr size_t CHALLENGE_PREFIX_SIZE = 5;
using Challenge = std::array<u8, CHALLENGE_PREFIX_SIZE + 16>;
using namespace std::chrono;
using Deadline = steady_clock::time_point;

bool setNonBlocking(sock_t sock, bool enabled)
{
#ifdef _WIN32
	u_long mode = enabled;
	return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
	const int flags = fcntl(sock, F_GETFL, 0);
	return flags >= 0 && fcntl(sock, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
#endif
}

int waitForSocket(sock_t sock, bool writing, int timeoutMs)
{
	if (!VALID(sock))
		return -1;
#ifdef _WIN32
	fd_set ready;
	FD_ZERO(&ready);
	FD_SET(sock, &ready);
	fd_set errors = ready;
	timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
	const int result = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr,
			writing ? &errors : nullptr, &timeout);
	return result > 0 && writing && FD_ISSET(sock, &errors) ? -1 : result;
#else
	// poll allows descriptors above FD_SETSIZE and reports hangups as readiness.
	pollfd fd{ sock, static_cast<short>(writing ? POLLOUT : POLLIN), 0 };
	const int result = poll(&fd, 1, timeoutMs);
	if (result < 0 && errno == EINTR)
		return 0;
	return (fd.revents & POLLNVAL) != 0 ? -1 : result;
#endif
}

int waitUntil(sock_t sock, bool writing, Deadline deadline, const std::atomic<bool>& stop)
{
	while (!stop)
	{
		const auto remaining = ceil<milliseconds>(deadline - steady_clock::now());
		if (remaining <= milliseconds(0))
			break;
		const int interval = static_cast<int>(std::min(remaining, milliseconds(100)).count());
		const int result = waitForSocket(sock, writing, interval);
		if (result != 0)
			return result;
	}
	return 0;
}

bool retrySocketIO()
{
	const int error = get_last_error();
#ifdef _WIN32
	return error == L_EWOULDBLOCK || error == WSAEINTR;
#else
	return error == L_EWOULDBLOCK || error == L_EAGAIN || error == EINTR;
#endif
}

void putU16(std::vector<u8>& out, u16 v)
{
	out.push_back((u8)(v & 0xff));
	out.push_back((u8)((v >> 8) & 0xff));
}

void putU32(std::vector<u8>& out, u32 v)
{
	out.push_back((u8)(v & 0xff));
	out.push_back((u8)((v >> 8) & 0xff));
	out.push_back((u8)((v >> 16) & 0xff));
	out.push_back((u8)((v >> 24) & 0xff));
}

u16 getU16(const u8* p)
{
	return (u16)(p[0] | (p[1] << 8));
}

u32 getU32(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

bool validItem(u8 kind, size_t size, size_t& total)
{
	if (kind != KIND_FLASH && kind != KIND_VMU)
		return false;
	if (size == 0 || size > MAX_ITEM_SIZE)
		return false;
	total += size;
	return total <= MAX_TOTAL_SIZE;
}

bool sendAll(sock_t sock, const u8* data, size_t len, Deadline deadline, const std::atomic<bool>& stop)
{
#ifdef MSG_NOSIGNAL
	constexpr int flags = MSG_NOSIGNAL;
#else
	constexpr int flags = 0;
#endif
	while (len > 0 && !stop && steady_clock::now() < deadline)
	{
		int sent = (int)send(sock, (const char*)data, (int)len, flags);
		if (sent <= 0)
		{
			if (sent == 0 || !retrySocketIO() || waitUntil(sock, true, deadline, stop) <= 0)
				return false;
			continue;
		}
		data += sent;
		len -= sent;
	}
	return len == 0;
}

bool recvAll(sock_t sock, u8* data, size_t len, Deadline deadline, const std::atomic<bool>& stop)
{
	while (len > 0 && !stop && steady_clock::now() < deadline)
	{
		int received = (int)recv(sock, (char*)data, (int)len, 0);
		if (received <= 0)
		{
			if (received == 0 || !retrySocketIO() || waitUntil(sock, false, deadline, stop) <= 0)
				return false;
			continue;
		}
		data += received;
		len -= received;
	}
	return len == 0;
}

} // namespace

bool encode(const std::vector<Item>& items, std::vector<u8>& out)
{
	if (items.size() > MAX_ITEMS)
		return false;
	size_t total = 0;
	for (const Item& item : items)
		if (!validItem(item.kind, item.data.size(), total))
			return false;

	out.clear();
	out.push_back('H');
	out.push_back('C');
	out.push_back('S');
	out.push_back('1');
	putU16(out, FORMAT_VERSION);
	putU16(out, (u16)items.size());
	for (const Item& item : items)
	{
		out.push_back(item.kind);
		out.push_back(item.bus);
		out.push_back(item.port);
		putU32(out, (u32)item.data.size());
		out.insert(out.end(), item.data.begin(), item.data.end());
	}
	return true;
}

bool decode(const u8* data, size_t len, std::vector<Item>& items)
{
	items.clear();
	if (data == nullptr || len < 8)
		return false;
	if (data[0] != 'H' || data[1] != 'C' || data[2] != 'S' || data[3] != '1')
		return false;
	if (getU16(data + 4) != FORMAT_VERSION)
		return false;
	const size_t count = getU16(data + 6);
	if (count > MAX_ITEMS)
		return false;

	size_t offset = 8;
	size_t total = 0;
	std::vector<Item> parsed;
	for (size_t i = 0; i < count; i++)
	{
		if (len - offset < 7)
			return false;
		Item item;
		item.kind = data[offset];
		item.bus = data[offset + 1];
		item.port = data[offset + 2];
		const u32 size = getU32(data + offset + 3);
		offset += 7;
		if (!validItem(item.kind, size, total))
			return false;
		if (len - offset < size)
			return false;
		item.data.assign(data + offset, data + offset + size);
		offset += size;
		parsed.push_back(std::move(item));
	}
	if (offset != len)
		return false;
	items = std::move(parsed);
	return true;
}

static bool sendRequest(sock_t sock, u32 protocol, Deadline deadline, const std::atomic<bool>& stop)
{
	u8 request[8] = { 'H', 'C', 'R', 'Q', 0, 0, 0, 0 };
	request[4] = (u8)(protocol & 0xff);
	request[5] = (u8)((protocol >> 8) & 0xff);
	request[6] = (u8)((protocol >> 16) & 0xff);
	request[7] = (u8)((protocol >> 24) & 0xff);
	return sendAll(sock, request, sizeof(request), deadline, stop);
}

static bool receiveRequest(sock_t sock, u32& protocol, Deadline deadline, const std::atomic<bool>& stop)
{
	u8 request[8];
	if (!recvAll(sock, request, sizeof(request), deadline, stop))
		return false;
	if (request[0] != 'H' || request[1] != 'C' || request[2] != 'R' || request[3] != 'Q')
		return false;
	protocol = getU32(request + 4);
	return true;
}

static bool sendItems(sock_t sock, const std::vector<Item>& items, Deadline deadline, const std::atomic<bool>& stop)
{
	std::vector<u8> buffer;
	if (!encode(items, buffer))
		return false;
	return sendAll(sock, buffer.data(), buffer.size(), deadline, stop);
}

static bool receiveItems(sock_t sock, std::vector<Item>& items, Deadline deadline, const std::atomic<bool>& stop)
{
	u8 header[8];
	if (!recvAll(sock, header, sizeof(header), deadline, stop))
		return false;
	std::vector<u8> buffer(header, header + sizeof(header));
	// Re-read the stream item by item, then validate the whole buffer with
	// the same strict decoder used for the in-memory format.
	if (buffer[0] != 'H' || buffer[1] != 'C' || buffer[2] != 'S' || buffer[3] != '1'
			|| getU16(buffer.data() + 4) != FORMAT_VERSION)
		return false;
	const size_t count = getU16(buffer.data() + 6);
	if (count > MAX_ITEMS)
		return false;
	size_t total = 0;
	for (size_t i = 0; i < count; i++)
	{
		u8 itemHeader[7];
		if (!recvAll(sock, itemHeader, sizeof(itemHeader), deadline, stop))
			return false;
		const u32 size = getU32(itemHeader + 3);
		if (!validItem(itemHeader[0], size, total))
			return false;
		buffer.insert(buffer.end(), itemHeader, itemHeader + sizeof(itemHeader));
		const size_t payloadOffset = buffer.size();
		buffer.resize(payloadOffset + size);
		if (!recvAll(sock, buffer.data() + payloadOffset, size, deadline, stop))
			return false;
	}
	return decode(buffer.data(), buffer.size(), items);
}

bool waitForReadable(sock_t sock, int timeoutMs)
{
	return waitForSocket(sock, false, timeoutMs) > 0;
}

static bool prepareTransfer(sock_t tcp)
{
#ifdef SO_NOSIGPIPE
	const int enabled = 1;
	if (setsockopt(tcp, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&enabled, sizeof(enabled)) != 0)
		return false;
#endif
	// Keep the calls nonblocking even if readiness changes before send or recv.
	return setNonBlocking(tcp, true);
}

ServeResult serveGuest(sock_t tcp, sock_t udp, sockaddr_in& guest, u32 protocol,
		const std::vector<Item>& items, const std::atomic<bool>& stop)
{
	if (!prepareTransfer(tcp))
		return ServeResult::Retry;
	const auto requestDeadline = steady_clock::now() + seconds(2);
	u32 requestedProtocol = 0;
	if (!receiveRequest(tcp, requestedProtocol, requestDeadline, stop) || stop)
		return ServeResult::Retry;
	if (requestedProtocol != protocol)
		return ServeResult::IncompatibleVersion;

	Challenge challenge{};
	std::random_device random;
	std::uniform_int_distribution<int> byte(0, 255);
	for (size_t i = CHALLENGE_PREFIX_SIZE; i < challenge.size(); ++i)
		challenge[i] = static_cast<u8>(byte(random));
	if (!sendAll(tcp, challenge.data(), challenge.size(), requestDeadline, stop))
		return ServeResult::Retry;

	const auto proofDeadline = steady_clock::now() + seconds(5);
	while (waitUntil(udp, false, proofDeadline, stop) > 0 && !stop)
	{
		sockaddr_in source{};
		socklen_t size = sizeof(source);
		char packet[challenge.size() + 1];
		const int received = recvfrom(udp, packet, sizeof(packet), 0, (sockaddr*)&source, &size);
		// A valid echo binds the TCP requester to the UDP endpoint before any saves are released.
		if (received != static_cast<int>(challenge.size()) || source.sin_port == 0
				|| source.sin_addr.s_addr != guest.sin_addr.s_addr
				|| std::memcmp(packet, challenge.data(), challenge.size()) != 0)
			continue;
		const auto bundleDeadline = steady_clock::now() + seconds(10);
		u8 ack = 0;
		if (!sendItems(tcp, items, bundleDeadline, stop)
				|| !recvAll(tcp, &ack, 1, steady_clock::now() + seconds(10), stop) || ack != 1 || stop)
			return ServeResult::Retry;
		guest.sin_port = source.sin_port;
		return ServeResult::Complete;
	}
	return ServeResult::Retry;
}

bool fetchFromHost(sock_t tcp, sock_t udp, const sockaddr_in& host, u32 protocol,
		std::vector<Item>& items, const std::atomic<bool>& stop)
{
	if (!prepareTransfer(tcp))
		return false;
	const auto requestDeadline = steady_clock::now() + seconds(2);
	Challenge challenge;
	if (!sendRequest(tcp, protocol, requestDeadline, stop)
			|| !recvAll(tcp, challenge.data(), challenge.size(), requestDeadline, stop))
		return false;

	const auto proofDeadline = steady_clock::now() + seconds(5);
	while (!stop && steady_clock::now() < proofDeadline)
	{
		sendto(udp, (const char*)challenge.data(), challenge.size(), 0, (const sockaddr*)&host, sizeof(host));
		const auto retryAt = std::min(proofDeadline, steady_clock::now() + milliseconds(250));
		const int ready = waitUntil(tcp, false, retryAt, stop);
		if (ready < 0)
			return false;
		if (ready > 0)
			return receiveItems(tcp, items, steady_clock::now() + seconds(10), stop) && !stop;
	}
	return false;
}

bool acknowledgeHost(sock_t tcp, const std::atomic<bool>& stop)
{
	const u8 ack = 1;
	return sendAll(tcp, &ack, 1, steady_clock::now() + seconds(10), stop);
}

sock_t connectWithTimeout(const sockaddr_in& target, int timeoutMs, const std::atomic<bool>& stop)
{
	const sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
	if (!VALID(sock))
		return INVALID_SOCKET;
	if (setNonBlocking(sock, true) && !stop)
	{
		bool connected = connect(sock, (const sockaddr*)&target, sizeof(target)) == 0;
		// Winsock reports WSAEWOULDBLOCK while a nonblocking connection is pending.
#ifdef _WIN32
		constexpr int connectPending = L_EWOULDBLOCK;
#else
		constexpr int connectPending = L_EINPROGRESS;
#endif
		if (!connected && get_last_error() == connectPending
				&& waitUntil(sock, true, steady_clock::now() + milliseconds(timeoutMs), stop) > 0)
		{
			int error = 0;
			socklen_t errorSize = sizeof(error);
			connected = getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &errorSize) == 0 && error == 0;
		}
		if (connected && !stop && setNonBlocking(sock, false))
			return sock;
	}
	closesocket(sock);
	return INVALID_SOCKET;
}

} // namespace sessionstate
