<!-- Portions Copyright 2026 The Hollycast Authors -->

# Crash reporting

Hollycast no longer uploads crash minidumps to Sentry. Local Breakpad minidumps and diagnostic logs are still kept on the device.

## Find the local crash files on Windows

1. After an unexpected crash in a Windows build with Breakpad enabled, open `%TEMP%` in File Explorer.
2. Sort by modification time and look for a `.dmp` created around the crash. Check alongside it for the diagnostic log. The **Minidump saved to** message in Windows debugger output gives the exact path when a debugger captures it.
3. For a bug report, record the Hollycast build, the game or screen you were using, and what happened immediately before the crash. Keep the matching dump and log available for diagnosis.

[Source pull request #50](https://github.com/nexus382/Hollycast/pull/50)
