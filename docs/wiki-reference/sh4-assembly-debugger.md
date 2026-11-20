<!-- Portions Copyright 2026 The Hollycast Authors -->

# Assembly debugger

Load an offline game, then open **Tools > Assembly Debugger** or choose **Assembly Debugger** from the pause menu. Pause or resume execution, step through SH4 instructions, inspect registers, jump to a hexadecimal address, and set breakpoints. The debugger is unavailable during network play, multiboard emulation, achievement hardcore mode, or while the GDB server is enabled. Using it can slow emulation.

## Step through an offline game

1. Load an offline game, then open **Tools → Assembly Debugger** or select it in the pause menu. The current SH-4 instruction is selected and execution pauses.
2. Choose **Step** to execute the selected instruction, or select an address and choose **Add Breakpoint** to stop there later.
3. Choose **Resume** to run the game again, then use **Pause** in the debugger control window to inspect the current instruction. Choose **Close Debugger** when finished.

[Debugger controls](../assembly-debugger.md)

[Source pull request #51](https://github.com/nexus382/Hollycast/pull/51)
