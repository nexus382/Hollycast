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
#include "cloudflare_tunnel.h"
#include "cfg/option.h"
#include "oslib/http_client.h"
#include "stdclass.h"

#include <cstdio>
#include <fstream>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace remoteplay {
namespace {

std::mutex tunnelMutex;
TunnelStatus status_;
std::thread readerThread;
std::atomic<bool> readerStop { false };
#ifdef _WIN32
HANDLE processHandle = nullptr;
HANDLE jobHandle = nullptr;
HANDLE readPipe = nullptr;
#else
pid_t childPid = -1;
int readFd = -1;
#endif

bool fileExists(const std::string& path)
{
	if (path.empty())
		return false;
#ifdef _WIN32
	return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
	return access(path.c_str(), X_OK) == 0;
#endif
}

void setState(TunnelState state, const std::string& error = "")
{
	std::lock_guard<std::mutex> lock(tunnelMutex);
	status_.state = state;
	if (!error.empty() || state == TunnelState::Failed)
		status_.error = error;
}

void consumeOutput(const std::string& chunk)
{
	std::string url = parseQuickTunnelUrl(chunk);
	if (url.empty())
		return;
	std::lock_guard<std::mutex> lock(tunnelMutex);
	status_.publicUrl = url;
	status_.state = TunnelState::Running;
	status_.error.clear();
}

void joinReader()
{
	readerStop = true;
	if (readerThread.joinable())
		readerThread.join();
}

} // namespace

std::string parseQuickTunnelUrl(const std::string& text)
{
	size_t start = text.find("https://");
	while (start != std::string::npos)
	{
		size_t end = text.find_first_of(" \t\r\n\"'|", start);
		std::string url = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (url.find(".trycloudflare.com") != std::string::npos)
			return url;
		start = text.find("https://", start + 1);
	}
	return "";
}

std::string findCloudflared()
{
	std::string configured = (std::string)config::RemotePlayCloudflaredPath;
	if (fileExists(configured))
		return configured;
	std::string installed = installedCloudflaredPath();
	if (fileExists(installed))
		return installed;
	const char *pathEnv = std::getenv("PATH");
	if (pathEnv != nullptr)
	{
		std::string path = pathEnv;
		size_t pos = 0;
#ifdef _WIN32
		const char sep = ';';
		const char *names[] = { "cloudflared.exe", "cloudflared" };
#else
		const char sep = ':';
		const char *names[] = { "cloudflared" };
#endif
		while (pos <= path.size())
		{
			size_t next = path.find(sep, pos);
			std::string dir = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
			for (const char *name : names)
			{
				std::string candidate = dir + "/" + name;
				if (fileExists(candidate))
					return candidate;
			}
			if (next == std::string::npos)
				break;
			pos = next + 1;
		}
	}
#ifndef _WIN32
	for (const char *candidate : { "/opt/homebrew/bin/cloudflared", "/usr/local/bin/cloudflared", "/usr/bin/cloudflared" })
		if (fileExists(candidate))
			return candidate;
#endif
	return "";
}

bool tunnelStart(int localPort, std::string& error)
{
	tunnelStop();
	std::string binary = findCloudflared();
	if (binary.empty())
	{
		error = "cloudflared not found. Install it (e.g. 'brew install cloudflared' or from cloudflare.com) or set its path in Remote Play settings.";
		setState(TunnelState::Failed, error);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(tunnelMutex);
		status_ = TunnelStatus{};
		status_.state = TunnelState::Starting;
		status_.binaryPath = binary;
	}
	readerStop = false;
	std::string url = "http://127.0.0.1:" + std::to_string(localPort);
#ifdef _WIN32
	SECURITY_ATTRIBUTES sa { sizeof(sa), nullptr, TRUE };
	HANDLE writePipe = nullptr;
	if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
	{
		error = "Could not create a pipe for cloudflared.";
		setState(TunnelState::Failed, error);
		return false;
	}
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOA si {};
	si.cb = sizeof(si);
	si.hStdOutput = writePipe;
	si.hStdError = writePipe;
	si.dwFlags = STARTF_USESTDHANDLES;
	PROCESS_INFORMATION pi {};
	std::string cmd = "\"" + binary + "\" tunnel --url " + url + " --no-autoupdate";
	if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
	{
		CloseHandle(readPipe); readPipe = nullptr;
		CloseHandle(writePipe);
		error = "Could not start cloudflared.";
		setState(TunnelState::Failed, error);
		return false;
	}
	CloseHandle(writePipe);
	CloseHandle(pi.hThread);
	processHandle = pi.hProcess;
	// Tie cloudflared to Hollycast: a kill-on-close job kills it when
	// this process exits for any reason, crash included.
	jobHandle = CreateJobObjectA(nullptr, nullptr);
	if (jobHandle != nullptr)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &info, sizeof(info));
		AssignProcessToJobObject(jobHandle, processHandle);
	}
	readerThread = std::thread([] {
		char buf[1024];
		DWORD read = 0;
		while (!readerStop && ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) && read > 0)
			consumeOutput(std::string(buf, read));
	});
#else
	int pipeFds[2];
	if (pipe(pipeFds) != 0)
	{
		error = "Could not create a pipe for cloudflared.";
		setState(TunnelState::Failed, error);
		return false;
	}
	fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
	fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
	pid_t pid = fork();
	if (pid < 0)
	{
		close(pipeFds[0]);
		close(pipeFds[1]);
		error = "Could not start cloudflared.";
		setState(TunnelState::Failed, error);
		return false;
	}
	if (pid == 0)
	{
		// Supervisor process: cloudflared must not outlive Hollycast.
		// It watches its parent and kills cloudflared when Hollycast
		// exits, crashes or is force-quit (no cleanup runs then).
		pid_t hollyPid = getppid();
		pid_t cf = fork();
		if (cf == 0)
		{
			dup2(pipeFds[1], STDOUT_FILENO);
			dup2(pipeFds[1], STDERR_FILENO);
			close(pipeFds[0]);
			close(pipeFds[1]);
			execlp(binary.c_str(), binary.c_str(), "tunnel", "--url", url.c_str(), "--no-autoupdate", (char *)nullptr);
			_exit(127);
		}
		if (cf < 0)
			_exit(127);
		// Drop everything inherited from Hollycast (sockets, files).
		long maxFd = sysconf(_SC_OPEN_MAX);
		if (maxFd < 0 || maxFd > 4096)
			maxFd = 4096;
		for (int fd = 3; fd < maxFd; fd++)
			close(fd);
		static volatile sig_atomic_t stopFlag = 0;
		signal(SIGTERM, [](int) { stopFlag = 1; });
		signal(SIGINT, [](int) { stopFlag = 1; });
		while (true)
		{
			int wstatus = 0;
			if (waitpid(cf, &wstatus, WNOHANG) == cf)
				_exit(0);	// cloudflared ended on its own
			if (stopFlag || getppid() != hollyPid || kill(hollyPid, 0) != 0)
			{
				kill(cf, SIGTERM);
				waitpid(cf, nullptr, 0);
				_exit(0);
			}
			struct timespec ts { 0, 200 * 1000 * 1000 };
			nanosleep(&ts, nullptr);
		}
	}
	close(pipeFds[1]);
	childPid = pid;
	readFd = pipeFds[0];
	readerThread = std::thread([] {
		char buf[1024];
		ssize_t n;
		std::string pending;
		while (!readerStop && (n = read(readFd, buf, sizeof(buf))) > 0)
		{
			pending += std::string(buf, (size_t)n);
			consumeOutput(pending);
			if (pending.size() > 65536)
				pending.clear();
		}
		if (!readerStop)
			setState(TunnelState::Failed, "cloudflared exited. Check that it is installed and up to date.");
	});
#endif
	return true;
}

void tunnelStop()
{
#ifdef _WIN32
	if (processHandle != nullptr)
	{
		TerminateProcess(processHandle, 0);
		CloseHandle(processHandle);
		processHandle = nullptr;
	}
	if (jobHandle != nullptr)
	{
		CloseHandle(jobHandle);
		jobHandle = nullptr;
	}
	if (readPipe != nullptr)
	{
		CloseHandle(readPipe);
		readPipe = nullptr;
	}
	joinReader();
#else
	if (childPid > 0)
	{
		kill(childPid, SIGTERM);
		waitpid(childPid, nullptr, 0);
		childPid = -1;
	}
	if (readFd >= 0)
	{
		close(readFd);
		readFd = -1;
	}
	joinReader();
#endif
	std::lock_guard<std::mutex> lock(tunnelMutex);
	if (status_.state != TunnelState::Failed)
		status_.state = TunnelState::Stopped;
	status_.publicUrl.clear();
}

TunnelStatus tunnelStatus()
{
	std::lock_guard<std::mutex> lock(tunnelMutex);
	TunnelStatus copy = status_;
	if (copy.binaryPath.empty())
		copy.binaryPath = findCloudflared();
	return copy;
}

std::string installedCloudflaredPath()
{
#ifdef _WIN32
	return get_writable_data_path("cloudflared.exe");
#else
	return get_writable_data_path("cloudflared");
#endif
}

std::string cloudflaredDownloadUrl()
{
	static const char base[] = "https://github.com/cloudflare/cloudflared/releases/latest/download/";
#if defined(_WIN32)
#	if defined(_M_ARM64) || defined(__aarch64__)
	return std::string(base) + "cloudflared-windows-arm64.exe";
#	else
	return std::string(base) + "cloudflared-windows-amd64.exe";
#	endif
#elif defined(__APPLE__)
#	if defined(__aarch64__)
	return std::string(base) + "cloudflared-darwin-arm64.tgz";
#	else
	return std::string(base) + "cloudflared-darwin-amd64.tgz";
#	endif
#else
#	if defined(__aarch64__)
	return std::string(base) + "cloudflared-linux-arm64";
#	else
	return std::string(base) + "cloudflared-linux-amd64";
#	endif
#endif
}

namespace {
std::mutex installMutex;
InstallStatus installStatus_;
std::thread installThread;

void setInstall(InstallState state, const std::string& message, const std::string& path = "")
{
	std::lock_guard<std::mutex> lock(installMutex);
	installStatus_.state = state;
	installStatus_.message = message;
	if (!path.empty())
		installStatus_.path = path;
}

bool writeFile(const std::string& path, const std::vector<u8>& data)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out.write((const char *)data.data(), (std::streamsize)data.size());
	return out.good();
}
} // namespace

void installCloudflared()
{
	{
		std::lock_guard<std::mutex> lock(installMutex);
		if (installStatus_.state == InstallState::Downloading || installStatus_.state == InstallState::Extracting)
			return;
	}
	if (installThread.joinable())
		installThread.join();
	installThread = std::thread([] {
		setInstall(InstallState::Downloading, "Downloading cloudflared from Cloudflare...");
		std::vector<u8> data;
		int status = http::get(cloudflaredDownloadUrl(), data);
		if (!http::success(status) || data.size() < (1u << 20))
		{
			setInstall(InstallState::Failed,
				"Download failed (HTTP " + std::to_string(status) + "). Check your connection and try again, "
				"or install cloudflared manually from cloudflare.com.");
			return;
		}
		std::string dest = installedCloudflaredPath();
#ifdef _WIN32
		if (!writeFile(dest, data))
		{
			setInstall(InstallState::Failed, "Could not write to " + dest);
			return;
		}
#elif defined(__APPLE__)
		setInstall(InstallState::Extracting, "Unpacking cloudflared...");
		std::string archive = dest + ".tgz";
		if (!writeFile(archive, data))
		{
			setInstall(InstallState::Failed, "Could not write to " + archive);
			return;
		}
		std::string dir = get_writable_data_path("");
		std::string cmd = "tar -xzf \"" + archive + "\" -C \"" + dir + "\"";
		int rc = std::system(cmd.c_str());
		std::remove(archive.c_str());
		if (rc != 0 || !fileExists(dest))
		{
			setInstall(InstallState::Failed, "Could not unpack cloudflared. Install it manually from cloudflare.com.");
			return;
		}
#else
		if (!writeFile(dest, data))
		{
			setInstall(InstallState::Failed, "Could not write to " + dest);
			return;
		}
#endif
#ifndef _WIN32
		chmod(dest.c_str(), 0755);
		if (!fileExists(dest))
		{
			setInstall(InstallState::Failed, "Installed file is not executable: " + dest);
			return;
		}
#endif
		setInstall(InstallState::Done, "cloudflared installed.", dest);
	});
}

InstallStatus installStatus()
{
	std::lock_guard<std::mutex> lock(installMutex);
	return installStatus_;
}

} // namespace remoteplay
