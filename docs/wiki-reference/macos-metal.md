<!-- Portions Copyright 2026 The Hollycast Authors -->

# Add native Metal rendering and per-pixel sorting on macOS

On standalone macOS, open **Settings → Video → Graphics API** and select **Metal (Experimental)** if offered. Per Strip and Per Triangle are available, with Per Pixel on devices that pass its separate capability check. Use another renderer for NAOMI 2.

## Try Metal

1. In a standalone macOS build with Metal support, open **Settings → Video → Graphics API** and choose **Metal (Experimental)** when it appears.
2. Choose **Per Triangle** or **Per Strip** for transparency sorting. Choose **Per Pixel** if your Mac offers it.
3. Start a Dreamcast game and check that gameplay displays. Select another graphics API before starting a NAOMI 2 game.

[Source pull request #43](https://github.com/nexus382/Hollycast/pull/43)
