<!-- Portions Copyright 2026 The Hollycast Authors -->

# Runtime Work Reduction

The change skips desktop VMU display work while VMU windows are disabled, avoids unchanged Google Play updater reads, shares one library-statistics scan per draw, and skips an unnecessary Metal depth-buffer round trip for opaque-only unsorted rendering.

## Turn off the VMU display

1. On a desktop build, start a Dreamcast game that updates its VMU LCD.
2. Open **On Screen Display → In-Game Display**. Use **Show VMU In-Game** to show the LCD. Where supported, **Move VMU** lets you reposition it.
3. Turn off **Show VMU In-Game** to hide the LCD and return to your game.

[Source pull request #79](https://github.com/nexus382/Hollycast/pull/79)
