// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace hikaru::frontend {
// load is performed on the game loader thread. requestReset is thread-safe;
// render, releaseGraphics and unload run on the thread owning the GL context.
void load(const std::string& path);
bool render();
void requestReset();
// Queues an emulated coin pulse; safe to call from the native menu thread.
void insertCredit(unsigned player = 0);
void pauseAudio();
void resumeAudio();
void releaseGraphics();
void unload();
}
