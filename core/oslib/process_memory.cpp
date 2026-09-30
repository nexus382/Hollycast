/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "process_memory.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <fstream>
#include <unistd.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/user.h>
#endif
#endif

namespace process_memory {

Stats queryCurrentProcess()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX counters {};
	if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
		return { static_cast<std::uint64_t>(counters.WorkingSetSize), true };
#elif defined(__APPLE__)
	mach_task_basic_info_data_t info {};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
		return { static_cast<std::uint64_t>(info.resident_size), true };
#elif defined(__linux__) || defined(__ANDROID__)
	std::ifstream statm("/proc/self/statm");
	std::uint64_t totalPages = 0;
	std::uint64_t residentPages = 0;
	if (statm >> totalPages >> residentPages)
	{
		const long pageSize = sysconf(_SC_PAGESIZE);
		if (pageSize > 0)
			return { residentPages * static_cast<std::uint64_t>(pageSize), true };
	}
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#if defined(__FreeBSD__)
	kinfo_proc info {};
	int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
#elif defined(__NetBSD__)
	kinfo_proc2 info {};
	int mib[] = { CTL_KERN, KERN_PROC2, KERN_PROC_PID, getpid(), static_cast<int>(sizeof(info)), 1 };
#else
	kinfo_proc info {};
	int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid(), static_cast<int>(sizeof(info)), 1 };
#endif
	size_t size = sizeof(info);
	if (sysctl(mib, sizeof(mib) / sizeof(mib[0]), &info, &size, nullptr, 0) == 0 && size == sizeof(info))
	{
		// These fields report current RSS in pages; getrusage's maxrss is a
		// lifetime high-water mark and cannot replace a resident-set sample.
#if defined(__FreeBSD__)
		const auto residentPages = info.ki_rssize;
#else
		const auto residentPages = info.p_vm_rssize;
#endif
		const long pageSize = sysconf(_SC_PAGESIZE);
		if (residentPages >= 0 && pageSize > 0)
			return { static_cast<std::uint64_t>(residentPages) * static_cast<std::uint64_t>(pageSize), true };
	}
#endif
	return {};
}

} // namespace process_memory
