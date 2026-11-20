<!-- Portions Copyright 2026 The Hollycast Authors -->

# Startup Update Check and Android Uploads

The startup update check is enabled by default and waits until the intro animation finishes. Change it in **Settings > Advanced**. The Android build job now fails when its APK is missing instead of silently skipping the upload.

## Steps

1. In a build that includes this change, open **Settings > Advanced** and turn on **Check for updates on startup**.
2. Relaunch Hollycast with an internet connection and let the intro finish. If an update is available on your selected track, the update dialog appears after the intro.
3. To turn off this behavior, clear **Check for updates on startup**.

[Source pull request #77](https://github.com/nexus382/Hollycast/pull/77)
