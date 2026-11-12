// Portions Copyright 2026 The Hollycast Authors
#include <winsock2.h>
#include "oslib/http_client.h"
#include "log/Log.h"
#include "stdclass.h"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char*, int, const char*, ...) {}
// The transport uses these existing string/logging utilities; no emulator state
// or production logger is needed by the loopback cancellation fixture.
const std::string defaultWs(" \0", 2);
size_t get_last_slash_pos(const std::string& path) { return path.find_last_of("/\\"); }

int main() {
    using namespace std::chrono_literals;
    WSADATA sockets{};
    if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) return 1;
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (server == INVALID_SOCKET || bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
            || listen(server, 1) != 0) return 1;
    int length = sizeof(address);
    if (getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) != 0) return 1;
    std::promise<void> accepted, release;
    auto incoming = accepted.get_future();
    auto unblock = release.get_future();
    std::thread listener([&] {
        SOCKET peer = accept(server, nullptr, nullptr);
        if (peer != INVALID_SOCKET) {
            char bytes[4096];
            recv(peer, bytes, sizeof(bytes), 0);
            accepted.set_value();
            unblock.wait(); // Deliberately withhold the HTTP response.
            closesocket(peer);
        }
    });
    http::init();
    std::atomic<bool> cancelled{false};
    auto request = std::async(std::launch::async, [&] {
        std::vector<u8> bytes;
        return http::get("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/delayed",
                         bytes, nullptr, nullptr, 15000, &cancelled);
    });
    const bool reachedServer = incoming.wait_for(3s) == std::future_status::ready;
    cancelled = true;
    const bool stoppedPromptly = request.wait_for(1s) == std::future_status::ready;
    release.set_value();
    closesocket(server);
    listener.join();
    const int status = request.get();
    http::term();
    WSACleanup();
    if (!reachedServer || !stoppedPromptly || status != 499) {
        std::cerr << "Cancellation did not interrupt the pending HTTP response\n";
        return 1;
    }
    std::cout << "Pending WinINet response cancelled and joined within one second\n";
    return 0;
}
