<!-- Portions Copyright 2026 The Hollycast Authors -->

# Fix theme browser HTTP initialization on Windows

A Windows fix makes **Browse themes** work when it’s the first feature you open after starting Hollycast. The fix also covers downloading previews and theme packages.

## Steps

1. Start an internet-connected Windows build that includes the fix.
2. Before using any other download feature, open **Settings > Library** and select **Browse themes** or **Download Themes**.
3. Open a theme preview and select **Download and install**. When it finishes, choose **Use theme** to apply it to the library.

[Source pull request #49](https://github.com/nexus382/Hollycast/pull/49)
