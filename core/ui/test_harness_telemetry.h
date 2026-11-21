// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "test_harness_protocol.h"
#ifdef USE_HIKARU
#include "hw/hikaru/valkyrie/engine.h"
#endif

namespace test_harness::telemetry {

void initialize();
void configure(const Json& profile);
std::uint64_t completedGuestFrames();
#ifdef USE_HIKARU
void hikaruFrame(const HikaruEngineInputs& inputs);
#endif
Json live();
Json readMemory(const Json& args);

} // namespace test_harness::telemetry
