// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
class Renderer;
void metalCreateContext(void *window, void *display);
Renderer *rend_Metal();
Renderer *rend_Metal_OIT();
bool metalIsAvailable();
bool metalSupportsPerPixel();
