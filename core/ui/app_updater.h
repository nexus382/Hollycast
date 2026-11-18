// Portions Copyright 2026 The Hollycast Authors
#pragma once

namespace app_updates {
// Called on the UI thread after ImGui::NewFrame; startup results wait for an idle library.
void draw(bool idle);
void open();
void stop();
}
