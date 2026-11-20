<!-- Portions Copyright 2026 The Hollycast Authors -->

# Import selected Flycast dev updates through 2026-10-16

This Dev update brings in selected Flycast fixes for graphics, input, networking, disc handling, and save states. It adds Lua disc controls and improves Stretch to Fill. An older build that only reads V62 cannot open a new V63 state. Keep a copy of your existing saves when changing versions.

## Try Stretch to Fill and preserve your saves

1. Back up your save states and VMU files before switching builds. Keep a separate copy of any V62 state you still want to use with an older build.
2. Open a standalone Dev build containing PR #16 and start a game. Use a wide window or display so the change in aspect ratio is visible.
3. Open **Settings → Video → Aspect Ratio** and enable **Stretch to Fill**. If **Super Widescreen** is on, turn it off first. Return to the game and check that the image fills the window or screen. Turn **Stretch to Fill** off to restore your previous aspect-ratio settings.

[Source pull request #16](https://github.com/nexus382/Hollycast/pull/16)
