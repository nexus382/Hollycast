// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <string>
class Serializer;
class Deserializer;
namespace hikaru { struct GameDescriptor; struct CardReaderInfo; }
namespace hikaru::frontend {
// Static descriptor of the loaded title, or null; safe for native settings UI.
const GameDescriptor* loadedGame();
// load is performed on the game loader thread. requestReset is thread-safe;
// render, releaseGraphics and unload run on the thread owning the GL context.
void load(const std::string& path);
bool render();
// Called after the host swaps the frame; emits a queued LoadState event for its first presented generation.
void framePresented();
// Saves wait for the current frame. Loads are validated immediately and applied
// on the next render frame, where the graphics context is guaranteed current.
void serialize(Serializer& state);
void deserialize(Deserializer& state);
bool stateReady();
void requestReset();
// Applies a selected country only after complete cabinet settings are idle.
void requestCabinetRegion();
// Queues an emulated coin pulse; safe to call from the native menu thread.
void insertCredit(unsigned player = 0);
// Card controls share the device lock with MIE serial IO and save states.
bool cardReaderAvailable();
CardReaderInfo cardReaderInfo();
void cardAction();
void selectCard(const std::string& filename);
void pauseAudio();
void resumeAudio();
void releaseGraphics();
// Called while the old GL context is still current; preserves the live game.
void suspendGraphics();
void unload();
}
