// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <cstdlib>
#include <stdexcept>

namespace test_harness {

inline constexpr char ControllerApiName[] = "Hollycast Test Harness";

#ifdef HOLLYCAST_TEST_HARNESS
bool controllerInputOwned();
void prepareStorage();
void poll();
void beforePresent();
void presented();
void recordRewind();
void releaseInputs();
#else
inline bool controllerInputOwned() { return false; }
inline void prepareStorage()
{
	// Refuse a harness launch before an ordinary binary can open user saves.
	if (const char *root = std::getenv("HOLLYCAST_HARNESS_DIR"); root && *root)
		throw std::runtime_error("This build does not include HOLLYCAST_TEST_HARNESS");
}
inline void poll() {}
inline void beforePresent() {}
inline void presented() {}
inline void recordRewind() {}
inline void releaseInputs() {}
#endif

} // namespace test_harness
