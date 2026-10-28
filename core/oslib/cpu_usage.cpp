/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "cpu_usage.h"

#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sched.h>
#endif
#endif

namespace cpu_usage {

namespace {

struct Snapshot {
	std::uint64_t processMicroseconds = 0;
	std::uint64_t systemTotalTicks = 0;
	std::uint64_t systemBusyTicks = 0;
	std::uint64_t wallMicroseconds = 0;
	std::uint32_t logicalProcessorCount = 0;
	bool processValid = false;
	bool systemValid = false;
};

std::uint64_t getWallMicroseconds()
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

#if defined(_WIN32)
std::uint64_t fileTimeToMicroseconds(const FILETIME& fileTime)
{
	ULARGE_INTEGER value { fileTime.dwLowDateTime, fileTime.dwHighDateTime };
	return value.QuadPart / 10;
}
#endif

Snapshot readSnapshot()
{
	Snapshot snapshot;
	snapshot.wallMicroseconds = getWallMicroseconds();

#if defined(_WIN32)
	SYSTEM_INFO systemInfo {};
	GetSystemInfo(&systemInfo);
	snapshot.logicalProcessorCount = std::max<DWORD>(systemInfo.dwNumberOfProcessors, 1);

	FILETIME idleTime {};
	FILETIME kernelTime {};
	FILETIME userTime {};
	if (GetSystemTimes(&idleTime, &kernelTime, &userTime))
	{
		const std::uint64_t idle = fileTimeToMicroseconds(idleTime);
		const std::uint64_t kernel = fileTimeToMicroseconds(kernelTime);
		const std::uint64_t user = fileTimeToMicroseconds(userTime);
		snapshot.systemTotalTicks = kernel + user;
		snapshot.systemBusyTicks = snapshot.systemTotalTicks - std::min(idle, snapshot.systemTotalTicks);
		snapshot.systemValid = true;
	}

	FILETIME creationTime {};
	FILETIME exitTime {};
	FILETIME processKernelTime {};
	FILETIME processUserTime {};
	if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &processKernelTime, &processUserTime))
	{
		snapshot.processMicroseconds = fileTimeToMicroseconds(processKernelTime) + fileTimeToMicroseconds(processUserTime);
		snapshot.processValid = true;
	}
#elif defined(__APPLE__)
	const long processorCount = sysconf(_SC_NPROCESSORS_ONLN);
	snapshot.logicalProcessorCount = static_cast<std::uint32_t>(std::max(processorCount, 1L));

	host_cpu_load_info_data_t cpuInfo {};
	mach_msg_type_number_t cpuInfoCount = HOST_CPU_LOAD_INFO_COUNT;
	const mach_port_t host = mach_host_self();
	if (host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpuInfo), &cpuInfoCount) == KERN_SUCCESS)
	{
		snapshot.systemTotalTicks = static_cast<std::uint64_t>(cpuInfo.cpu_ticks[CPU_STATE_USER])
			+ cpuInfo.cpu_ticks[CPU_STATE_NICE] + cpuInfo.cpu_ticks[CPU_STATE_SYSTEM] + cpuInfo.cpu_ticks[CPU_STATE_IDLE];
		snapshot.systemBusyTicks = static_cast<std::uint64_t>(cpuInfo.cpu_ticks[CPU_STATE_USER])
			+ cpuInfo.cpu_ticks[CPU_STATE_NICE] + cpuInfo.cpu_ticks[CPU_STATE_SYSTEM];
		snapshot.systemValid = true;
	}
	mach_port_deallocate(mach_task_self(), host);
#elif defined(__linux__) || defined(__ANDROID__)
	const long processorCount = sysconf(_SC_NPROCESSORS_ONLN);
	snapshot.logicalProcessorCount = static_cast<std::uint32_t>(std::max(processorCount, 1L));

	std::ifstream systemStat("/proc/stat");
	std::string label;
	std::uint64_t user = 0;
	std::uint64_t nice = 0;
	std::uint64_t system = 0;
	std::uint64_t idle = 0;
	std::uint64_t ioWait = 0;
	std::uint64_t irq = 0;
	std::uint64_t softIrq = 0;
	std::uint64_t steal = 0;
	if (systemStat >> label >> user >> nice >> system >> idle >> ioWait >> irq >> softIrq >> steal && label == "cpu")
	{
		snapshot.systemTotalTicks = user + nice + system + idle + ioWait + irq + softIrq + steal;
		snapshot.systemBusyTicks = snapshot.systemTotalTicks - idle - ioWait;
		snapshot.systemValid = true;
	}

	std::ifstream processStat("/proc/self/stat");
	std::string processLine;
	if (std::getline(processStat, processLine))
	{
		const std::size_t nameEnd = processLine.rfind(')');
		if (nameEnd != std::string::npos)
		{
			std::istringstream fields(processLine.substr(nameEnd + 2));
			char state = 0;
			std::uint64_t ignored = 0;
			std::uint64_t userTicks = 0;
			std::uint64_t systemTicks = 0;
			fields >> state;
			for (int i = 0; i < 10 && fields; ++i)
				fields >> ignored;
			if (fields >> userTicks >> systemTicks)
			{
				const long ticksPerSecond = sysconf(_SC_CLK_TCK);
				if (ticksPerSecond > 0)
				{
					snapshot.processMicroseconds = (userTicks + systemTicks) * 1000000 / static_cast<std::uint64_t>(ticksPerSecond);
					snapshot.processValid = true;
				}
			}
		}
	}
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	const long processorCount = sysconf(_SC_NPROCESSORS_ONLN);
	if (processorCount > 0)
		snapshot.logicalProcessorCount = static_cast<std::uint32_t>(processorCount);

	// NetBSD exports fixed-width counters; FreeBSD and OpenBSD export longs.
	// Request one aggregate across all CPUs, including interrupt/spin work.
#if defined(__NetBSD__)
	std::uint64_t ticks[CPUSTATES] {};
#else
	long ticks[CPUSTATES] {};
#endif
	size_t size = sizeof(ticks);
#if defined(__FreeBSD__)
	const bool systemRead = sysctlbyname("kern.cp_time", ticks, &size, nullptr, 0) == 0;
#else
#if defined(__NetBSD__)
	int mib[] = { CTL_KERN, KERN_CP_TIME };
#else
	int mib[] = { CTL_KERN, KERN_CPTIME };
#endif
	const bool systemRead = sysctl(mib, 2, ticks, &size, nullptr, 0) == 0;
#endif
	if (systemRead && size == sizeof(ticks))
	{
		for (auto tick : ticks)
			snapshot.systemTotalTicks += static_cast<std::uint64_t>(tick);
		snapshot.systemBusyTicks = snapshot.systemTotalTicks - static_cast<std::uint64_t>(ticks[CP_IDLE]);
		snapshot.systemValid = true;
	}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	// RUSAGE_SELF includes exited workers; live-thread totals can go backwards
	// when a worker ends and would lose its CPU work between samples.
	rusage processInfo {};
	if (getrusage(RUSAGE_SELF, &processInfo) == 0)
	{
		snapshot.processMicroseconds = (static_cast<std::uint64_t>(processInfo.ru_utime.tv_sec)
			+ static_cast<std::uint64_t>(processInfo.ru_stime.tv_sec)) * 1000000
			+ processInfo.ru_utime.tv_usec + processInfo.ru_stime.tv_usec;
		snapshot.processValid = true;
	}
#endif

	return snapshot;
}

} // namespace

Stats queryCurrentProcessAndSystem()
{
	static Snapshot previous;
	static bool hasPrevious = false;

	const Snapshot current = readSnapshot();
	Stats result;
	result.logicalProcessorCount = current.logicalProcessorCount;
	if (hasPrevious)
	{
		if (current.systemValid && previous.systemValid && current.systemTotalTicks > previous.systemTotalTicks
			&& current.systemBusyTicks >= previous.systemBusyTicks)
		{
			const std::uint64_t totalDelta = current.systemTotalTicks - previous.systemTotalTicks;
			const std::uint64_t busyDelta = current.systemBusyTicks - previous.systemBusyTicks;
			result.systemPercent = std::min(100.f, static_cast<float>(busyDelta) * 100.f / static_cast<float>(totalDelta));
			result.systemValid = true;
		}
		if (current.processValid && previous.processValid && current.wallMicroseconds > previous.wallMicroseconds
			&& current.processMicroseconds >= previous.processMicroseconds && current.logicalProcessorCount != 0)
		{
			const std::uint64_t wallDelta = current.wallMicroseconds - previous.wallMicroseconds;
			const std::uint64_t processDelta = current.processMicroseconds - previous.processMicroseconds;
			const double capacityMicroseconds = static_cast<double>(wallDelta) * current.logicalProcessorCount;
			result.processPercent = std::min(100.f, static_cast<float>(static_cast<double>(processDelta) * 100.0 / capacityMicroseconds));
			result.processValid = true;
		}
	}

	previous = current;
	hasPrevious = true;
	return result;
}

} // namespace cpu_usage
