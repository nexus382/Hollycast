#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include <cstdarg>
#include <cstdio>
#include <string>

#include "oslib/i18n.h"
#include "oslib/oslib.h"

// The macOS app entry point supplies these functions in osx-main.mm, which is
// excluded from the unit-test target so gtest_main can provide main().
int darw_printf(const char *text, ...)
{
	va_list args;
	va_start(args, text);
	const int result = std::vprintf(text, args);
	va_end(args);
	return result;
}

void os_DoEvents()
{
}

void os_RunInstance(int argc, const char *argv[])
{
}

std::string os_PrecomposedString(std::string string)
{
	return string;
}

namespace hostfs {
std::string getScreenshotsPath()
{
	return {};
}
}

std::string i18n::getSystemLocale()
{
	return "en_US";
}

#ifdef DREAMPOTATO_INTEGRATED_MODE
std::string os_GetAppContainingDir()
{
	return "";
}

os_Process os_Process::start(const std::string& executable, const std::vector<std::string>& args)
{
	return os_Process();
}

bool os_Process::isRunning()
{
	return false;
}

void os_Process::terminate()
{
}
#endif
#endif

#if !defined(__ANDROID__) && !defined(__APPLE__)
#include <cstdlib>
#include "oslib/oslib.h"

[[noreturn]] void os_DebugBreak()
{
	std::abort();
}

void os_DoEvents()
{
}

void os_RunInstance(int argc, const char *argv[])
{
}

#ifdef DREAMPOTATO_INTEGRATED_MODE
std::string os_GetAppContainingDir()
{
	return "";
}

os_Process os_Process::start(const std::string& executable, const std::vector<std::string>& args)
{
	return os_Process();
}

bool os_Process::isRunning()
{
	return false;
}

void os_Process::terminate()
{
}
#endif

#ifdef _WIN32
void os_SetThreadName(const char *name)
{
}
const char *getThreadName()
{
	return "threadname";
}
#endif
#endif
