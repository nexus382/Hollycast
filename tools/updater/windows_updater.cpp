// Portions Copyright 2026 The Hollycast Authors
// Protocol v1: --manifest <private manifest.json> [--apply]. Bootstrap may hand a
// verified archive to its own newly packaged helper; keep this handoff compatible.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <zip.h>
#include "json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <iterator>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
using Internet = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
using Archive = std::unique_ptr<zip_t, decltype(&zip_discard)>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::wstring wide(const std::string& value) {
    require(value.find('\0') == std::string::npos, "Invalid text in update request.");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    require(size > 0, "Invalid UTF-8 in update request.");
    std::wstring result(size, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == size,
            "Cannot read update request text.");
    return result;
}
std::string hex(const unsigned char* bytes, size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
    return result;
}
class Sha256 {
public:
    Sha256() {
        try {
            require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA-256 is unavailable.");
            DWORD size = 0, received = 0;
            require(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &received, 0) >= 0,
                    "Cannot initialize SHA-256.");
            object.resize(size);
            require(BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0, "Cannot initialize SHA-256.");
        } catch (...) { if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
    }
    ~Sha256() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const void* bytes, DWORD size) {
        require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)), size, 0) >= 0, "Cannot verify package checksum.");
    }
    std::string finish() {
        std::array<unsigned char, 32> bytes{};
        require(BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) >= 0, "Cannot verify package checksum.");
        return hex(bytes.data(), bytes.size());
    }
private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
};
void status(const fs::path& work, const std::string& value) {
    const fs::path temporary = work / "status.tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << value; out.close();
    require(static_cast<bool>(out), "Cannot publish update progress.");
    for (int retry = 0; retry < 50; ++retry) {
        if (MoveFileExW(temporary.c_str(), (work / "status").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        Sleep(20); // A short UI read can briefly deny delete-sharing on the status file.
    }
    throw std::runtime_error("Cannot publish update progress.");
}
void cancelled(const fs::path& work) {
    require(!fs::exists(work / "cancel"), "Update cancelled.");
}
std::string fileHash(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(static_cast<bool>(input), "Cannot open the update package.");
    Sha256 hash;
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) hash.add(buffer.data(), static_cast<DWORD>(input.gcount()));
    require(input.eof(), "Cannot read the update package.");
    return hash.finish();
}
void download(const std::string& url, const fs::path& work, uint64_t expected, const std::string& digest) {
    const std::wstring address = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    require(WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &parts), "Invalid update URL.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    require(parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == INTERNET_DEFAULT_HTTPS_PORT && host == L"github.com",
            "Updates must come from the official HTTPS download URL.");
    Internet session(WinHttpOpen(L"Hollycast-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    require(static_cast<bool>(session), "Cannot start the update download.");
    WinHttpSetTimeouts(session.get(), 15000, 15000, 15000, 15000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    require(WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)), "Secure update downloads are unavailable.");
    Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), WinHttpCloseHandle);
    require(static_cast<bool>(connection), "Cannot connect to the release server.");
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), WinHttpCloseHandle);
    require(static_cast<bool>(request), "Cannot request the update package.");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    require(WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)), "Cannot enforce secure update redirects.");
    require(WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request.get(), nullptr), "The update download failed. Please retry.");
    DWORD code = 0, length = sizeof(code);
    require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &length, WINHTTP_NO_HEADER_INDEX)
            && code == 200, "The release server could not provide the update package.");
    std::ofstream out(work / "package.zip", std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "Cannot save the update package.");
    Sha256 hash;
    std::array<unsigned char, 65536> buffer{};
    uint64_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    for (;;) {
        cancelled(work);
        require(std::chrono::steady_clock::now() < deadline, "The update download timed out.");
        DWORD read = 0;
        require(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read), "The update download was interrupted.");
        if (!read) break;
        total += read;
        require(total <= expected, "The package exceeds its published size.");
        out.write(reinterpret_cast<const char*>(buffer.data()), read);
        require(static_cast<bool>(out), "Cannot save the update package.");
        hash.add(buffer.data(), read);
    }
    out.close();
    require(static_cast<bool>(out) && total == expected && hash.finish() == digest, "The update failed its size or SHA-256 verification.");
}
Archive openZip(const fs::path& path) {
    zip_source_t* source = zip_source_win32w_create(path.c_str(), 0, -1, nullptr);
    require(source != nullptr, "Cannot open the update archive.");
    zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, nullptr);
    if (!archive) { zip_source_free(source); throw std::runtime_error("The update is not a valid ZIP archive."); }
    const auto count = zip_get_num_entries(archive, 0);
    if (count <= 0 || count > 10000) { zip_discard(archive); throw std::runtime_error("The update archive has too many entries."); }
    return Archive(archive, zip_discard);
}
std::string basename(const char* raw) {
    require(raw != nullptr, "Invalid update archive entry.");
    std::string name(raw);
    require(name.size() <= 4096, "Invalid update archive filename.");
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}
zip_int64_t entry(zip_t* archive, const std::string& name, bool suffix = false) {
    zip_int64_t result = -1;
    for (zip_int64_t i = 0; i < zip_get_num_entries(archive, 0); ++i) {
        const auto candidate = basename(zip_get_name(archive, i, 0));
        const bool matches = suffix ? candidate.size() >= name.size() && candidate.compare(candidate.size() - name.size(), name.size(), name) == 0 : candidate == name;
        if (!matches) continue;
        require(result < 0, "The update contains ambiguous package entries.");
        result = i;
    }
    return result;
}
void extract(zip_t* archive, zip_int64_t index, const fs::path& output) {
    zip_stat_t info{};
    require(zip_stat_index(archive, index, 0, &info) == 0 && info.size > 0 && info.size <= 512ULL * 1024 * 1024, "Invalid update entry size.");
    std::unique_ptr<zip_file_t, decltype(&zip_fclose)> input(zip_fopen_index(archive, index, 0), zip_fclose);
    require(static_cast<bool>(input), "Cannot read the update archive.");
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    std::array<char, 65536> buffer{};
    uint64_t total = 0;
    for (;;) {
        const auto read = zip_fread(input.get(), buffer.data(), buffer.size());
        require(read >= 0, "Cannot extract the update package.");
        if (!read) break;
        total += read;
        require(total <= info.size, "Invalid unpacked update size.");
        out.write(buffer.data(), read);
        require(static_cast<bool>(out), "Cannot stage the update package.");
    }
    out.close();
    const int closed = zip_fclose(input.release());
    require(total == info.size && static_cast<bool>(out) && closed == 0, "The staged update is incomplete.");
}
Archive payload(const fs::path& work) {
    fs::path path = work / "package.zip";
    for (int depth = 0; depth < 3; ++depth) {
        auto archive = openZip(path);
        if (entry(archive.get(), "hollycast.exe") >= 0) return archive;
        const auto inner = entry(archive.get(), ".zip", true);
        require(inner >= 0, "The update does not contain Hollycast.");
        path = work / ("inner-" + std::to_string(depth) + ".zip");
        extract(archive.get(), inner, path);
        // Release each wrapper before opening its payload; no wrapper handle may
        // survive into application restart or terminal download cleanup.
        archive.reset();
    }
    throw std::runtime_error("The nested package format is unsupported.");
}
uint16_t machine(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    uint16_t mz = 0, result = 0;
    uint32_t offset = 0, signature = 0;
    file.read(reinterpret_cast<char*>(&mz), sizeof(mz));
    require(mz == IMAGE_DOS_SIGNATURE, "The update is not a Windows executable.");
    file.seekg(0x3c); file.read(reinterpret_cast<char*>(&offset), sizeof(offset));
    require(offset <= 1024 * 1024, "Invalid executable header.");
    file.seekg(offset); file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&result), sizeof(result));
    require(static_cast<bool>(file) && signature == IMAGE_NT_SIGNATURE, "Invalid executable header.");
    return result;
}
Handle launch(const fs::path& executable, const std::wstring& arguments, const fs::path& cwd, bool hidden) {
    std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = hidden ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0,
            nullptr, cwd.c_str(), &startup, &process), "Cannot start the updated application or helper.");
    CloseHandle(process.hThread);
    return Handle(process.hProcess, CloseHandle);
}
struct Replacement { fs::path target, prepared, backup; bool existed = false, installed = false; };
void discardPayloads(const fs::path& work) {
    if (work.empty()) return;
    for (const auto* name : {L"package.zip", L"inner-0.zip", L"inner-1.zip", L"inner-2.zip"}) {
        std::error_code ignored;
        fs::remove(work / name, ignored);
    }
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    fs::path work, target, cwd, transaction;
    Handle parent(nullptr, CloseHandle);
    std::vector<Replacement> changes;
    try {
        int argc = 0;
        std::unique_ptr<wchar_t*, decltype(&LocalFree)> argv(CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
        require(argv != nullptr && (argc == 3 || argc == 4) && std::wstring(argv.get()[1]) == L"--manifest", "Invalid updater arguments.");
        const bool apply = argc == 4 && std::wstring(argv.get()[3]) == L"--apply";
        require(argc == 3 || apply, "Unknown updater mode.");
        const fs::path manifest = fs::canonical(argv.get()[2]);
        work = manifest.parent_path();
        require(manifest.filename() == L"manifest.json" && fs::file_size(manifest) <= 65536, "Invalid update manifest.");
        wchar_t ownPath[32768];
        const DWORD ownLength = GetModuleFileNameW(nullptr, ownPath, static_cast<DWORD>(std::size(ownPath)));
        require(ownLength > 0 && ownLength < std::size(ownPath) && fs::equivalent(fs::path(ownPath).parent_path(), work), "The helper must run from its private staging folder.");
        std::ifstream stream(manifest, std::ios::binary);
        const auto request = Json::parse(stream);
        const auto url = request.at("url").get<std::string>();
        const auto digest = request.at("sha256").get<std::string>();
        const auto size = request.at("size").get<uint64_t>();
        require(url.rfind("https://github.com/nexus382/Hollycast/releases/download/", 0) == 0
                && url.find_first_of("\r\n\t ") == std::string::npos
                && std::regex_match(digest, std::regex("[a-f0-9]{64}")) && size > 0 && size <= 512ULL * 1024 * 1024,
                "Invalid official update metadata.");
        target = fs::canonical(fs::path(wide(request.at("target").get<std::string>())));
        cwd = fs::path(wide(request.at("cwd").get<std::string>()));
        if (!fs::is_directory(cwd)) cwd = target.parent_path();
        parent.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.at("pid").get<DWORD>()));
        require(static_cast<bool>(parent), "The application requesting this update is no longer running.");
        wchar_t parentPath[32768]; DWORD parentLength = static_cast<DWORD>(std::size(parentPath));
        require(QueryFullProcessImageNameW(parent.get(), 0, parentPath, &parentLength)
                && fs::equivalent(fs::path(parentPath), target), "The update target does not match the running application.");
        auto path = target.wstring();
        std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        Sha256 key; key.add(path.data(), static_cast<DWORD>(path.size() * sizeof(wchar_t)));
        const auto lockName = L"Local\\HollycastUpdate-" + wide(key.finish());
        bool acquired = false;
        auto release = [&acquired](void* handle) { if (handle) { if (acquired) ReleaseMutex(handle); CloseHandle(handle); } };
        std::unique_ptr<void, decltype(release)> lock(CreateMutexW(nullptr, FALSE, lockName.c_str()), release);
        require(static_cast<bool>(lock), "Cannot coordinate update installation.");
        const auto waited = WaitForSingleObject(lock.get(), apply ? 30000 : 0);
        require(waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED, "Another update is already in progress for this installation.");
        acquired = true;
        cancelled(work);
        if (!apply) {
            status(work, "Downloading\n");
            download(url, work, size, digest);
            auto archive = payload(work);
            const auto helper = entry(archive.get(), "hollycastupdater.exe");
            if (helper >= 0) {
                const auto fresh = work / "fresh-updater.exe";
                extract(archive.get(), helper, fresh);
                require(machine(fresh) == machine(target), "The update helper has the wrong architecture.");
                // Run the helper from the verified package, not from the directory being updated.
                // --apply prevents bootstrap recursion while allowing future helpers to evolve.
                launch(fresh, L"--manifest \"" + manifest.wstring() + L"\" --apply", work, true);
                return 0;
            }
        }
        require(fs::file_size(work / "package.zip") == size && fileHash(work / "package.zip") == digest, "The staged package failed verification.");
        auto archive = payload(work);
        std::array<unsigned char, 16> random{};
        require(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Cannot create update staging.");
        transaction = target.parent_path() / (L".hollycast-update-" + wide(hex(random.data(), random.size())));
        require(fs::create_directory(transaction), "The installation folder is not writable.");
        const std::array<std::string, 2> names{{"Hollycast.exe", "HollycastUpdater.exe"}};
        for (size_t i = 0; i < names.size(); ++i) {
            auto lower = names[i]; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto index = entry(archive.get(), lower);
            if (index < 0 && i == 1) continue; // Older track builds can predate the updater itself.
            require(index >= 0, "The update application is missing.");
            const auto prepared = transaction / fs::path(names[i]);
            extract(archive.get(), index, prepared);
            require(machine(prepared) == machine(target), "The update package has the wrong architecture.");
            const auto destination = i == 0 ? target : target.parent_path() / L"HollycastUpdater.exe";
            changes.push_back({destination, prepared, transaction / fs::path("previous-" + names[i]), fs::exists(destination), false});
        }
        archive.reset();
        cancelled(work);
        status(work, "Ready\n");
        DWORD parentState = WAIT_TIMEOUT;
        for (int second = 0; second < 180 && parentState == WAIT_TIMEOUT; ++second) {
            cancelled(work);
            parentState = WaitForSingleObject(parent.get(), 1000);
        }
        require(parentState == WAIT_OBJECT_0, "Hollycast did not close; no update was installed.");
        cancelled(work);
        // Only these managed binaries are replaced. Never overlay emu.cfg, data,
        // saves, artwork or library themes with arbitrary archive contents.
        for (auto& change : changes) {
            const BOOL installed = change.existed
                ? ReplaceFileW(change.target.c_str(), change.prepared.c_str(), change.backup.c_str(), 0, nullptr, nullptr)
                : MoveFileExW(change.prepared.c_str(), change.target.c_str(), MOVEFILE_WRITE_THROUGH);
            require(installed, "Cannot replace the application files; restoring the previous installation.");
            change.installed = true;
        }
        auto child = launch(target, L"", cwd, false);
        // CreateProcess can succeed before a missing DLL or startup error terminates
        // the replacement. Retain the backups until that early failure window passes.
        const DWORD started = WaitForSingleObject(child.get(), 3000);
        require(started == WAIT_TIMEOUT || started == WAIT_OBJECT_0, "Cannot verify application startup.");
        if (started == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            require(GetExitCodeProcess(child.get(), &exitCode) && exitCode == 0,
                    "The updated application could not start; restoring the previous installation.");
        }
        // Once a live replacement is accepted, a progress-file failure must not
        // attempt to overwrite its mapped executable or launch a second instance.
        try { status(work, "Done\n"); } catch (...) {}
        discardPayloads(work);
        return 0;
    } catch (const std::exception& error) {
        // Revert the whole managed binary set if any replacement or restart fails.
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            std::error_code ignored;
            if (fs::exists(it->backup, ignored)) MoveFileExW(it->backup.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            else if (it->installed && !it->existed) DeleteFileW(it->target.c_str());
        }
        try { if (!work.empty()) status(work, std::string("Error\n") + error.what()); } catch (...) {}
        try {
            if (parent && WaitForSingleObject(parent.get(), 0) == WAIT_OBJECT_0 && fs::exists(target))
                launch(target, L"", cwd, false);
        } catch (...) {}
        if (!transaction.empty()) {
            std::error_code ignored;
            fs::remove(transaction / "Hollycast.exe", ignored);
            fs::remove(transaction / "HollycastUpdater.exe", ignored);
            fs::remove(transaction, ignored); // Retain any backup whose restoration failed.
        }
        discardPayloads(work);
        return 1;
    }
}
