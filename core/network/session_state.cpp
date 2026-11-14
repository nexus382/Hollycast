// Portions Copyright 2026 The Hollycast Authors
#include "session_state.h"
#include <cstring>

namespace sessionstate
{
namespace
{

constexpr size_t MAX_ITEMS = 16;
constexpr size_t MAX_ITEM_SIZE = 256 * 1024;
constexpr size_t MAX_TOTAL_SIZE = 2 * 1024 * 1024;
constexpr u16 FORMAT_VERSION = 1;

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

bool sendAll(sock_t sock, const u8* data, size_t len)
{
	while (len > 0)
	{
		int sent = (int)send(sock, (const char*)data, (int)len, 0);
		if (sent <= 0)
			return false;
		data += sent;
		len -= sent;
	}
	return true;
}

bool recvAll(sock_t sock, u8* data, size_t len)
{
	while (len > 0)
	{
		int received = (int)recv(sock, (char*)data, (int)len, 0);
		if (received <= 0)
			return false;
		data += received;
		len -= received;
	}
	return true;
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

bool sendRequest(sock_t sock, u32 protocol)
{
	u8 request[8] = { 'H', 'C', 'R', 'Q', 0, 0, 0, 0 };
	request[4] = (u8)(protocol & 0xff);
	request[5] = (u8)((protocol >> 8) & 0xff);
	request[6] = (u8)((protocol >> 16) & 0xff);
	request[7] = (u8)((protocol >> 24) & 0xff);
	return sendAll(sock, request, sizeof(request));
}

bool receiveRequest(sock_t sock, u32& protocol)
{
	u8 request[8];
	if (!recvAll(sock, request, sizeof(request)))
		return false;
	if (request[0] != 'H' || request[1] != 'C' || request[2] != 'R' || request[3] != 'Q')
		return false;
	protocol = getU32(request + 4);
	return true;
}

bool sendItems(sock_t sock, const std::vector<Item>& items)
{
	std::vector<u8> buffer;
	if (!encode(items, buffer))
		return false;
	return sendAll(sock, buffer.data(), buffer.size());
}

bool receiveItems(sock_t sock, std::vector<Item>& items)
{
	u8 header[8];
	if (!recvAll(sock, header, sizeof(header)))
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
		if (!recvAll(sock, itemHeader, sizeof(itemHeader)))
			return false;
		const u32 size = getU32(itemHeader + 3);
		if (!validItem(itemHeader[0], size, total))
			return false;
		buffer.insert(buffer.end(), itemHeader, itemHeader + sizeof(itemHeader));
		const size_t payloadOffset = buffer.size();
		buffer.resize(payloadOffset + size);
		if (!recvAll(sock, buffer.data() + payloadOffset, size))
			return false;
	}
	return decode(buffer.data(), buffer.size(), items);
}

sock_t connectWithTimeout(const std::string& host, int port, int timeoutMs)
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* addresses = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0 || addresses == nullptr)
		return INVALID_SOCKET;

	sock_t sock = INVALID_SOCKET;
	for (addrinfo* addr = addresses; addr != nullptr; addr = addr->ai_next)
	{
		sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
		if (!VALID(sock))
			continue;
		sockaddr_in target{};
		std::memcpy(&target, addr->ai_addr, sizeof(target));
		target.sin_port = htons((u_short)port);

		set_non_blocking(sock);
		bool connected = connect(sock, (sockaddr*)&target, sizeof(target)) == 0;
		if (!connected && get_last_error() == L_EINPROGRESS)
		{
			fd_set writeFds;
			FD_ZERO(&writeFds);
			FD_SET(sock, &writeFds);
			timeval tv{};
			tv.tv_sec = timeoutMs / 1000;
			tv.tv_usec = (timeoutMs % 1000) * 1000;
			connected = select((int)sock + 1, nullptr, &writeFds, nullptr, &tv) > 0;
		}
		if (connected)
		{
			// Back to blocking mode for the transfer itself.
#ifdef _WIN32
			u_long blocking = 0;
			ioctlsocket(sock, FIONBIO, &blocking);
#else
			const int flags = fcntl(sock, F_GETFL, 0);
			fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif
			break;
		}
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	freeaddrinfo(addresses);
	return sock;
}

} // namespace sessionstate
