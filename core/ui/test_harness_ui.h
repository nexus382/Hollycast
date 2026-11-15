// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "test_harness_protocol.h"

namespace test_harness::ui {

void beginInspection();
bool inspectionReady();
Json inspection(const std::string& observationId);
void startAction(const Json& args);
void poll();
bool actionReady();
void release();
bool open();

} // namespace test_harness::ui
