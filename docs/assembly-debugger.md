<!-- Portions Copyright 2026 The Hollycast Authors -->
# Assembly debugger

Load an offline game, then open **Tools > Assembly Debugger** or select
**Assembly Debugger** from the in-game pause menu. Opening the browser pauses
execution and selects the next SH4 instruction at the program counter (PC).

- **Resume** runs the game with a small debugger control window. Select **Pause**
  there to stop and inspect the current instruction again.
- **Step** executes one instruction. A taken delayed branch and its delay slot
  execute together. A breakpoint on that slot stops before its branch, before
  either instruction changes CPU state. An untaken conditional branch steps to
  the following instruction, where a breakpoint can stop normally.
- **Current Instruction** returns to the PC, highlighted in yellow and marked `>`.
- **Go to Address** accepts an even hexadecimal address, with an optional `0x`
  prefix. Browsing changes neither PC nor game memory.
- Select a row and use **Add Breakpoint**, double-click a readable row, or use its
  context menu. `B` marks a breakpoint. The breakpoint list jumps to addresses and
  allows individual removal with `X`.
- The scrollbar covers a fixed 64 KiB address page. The page buttons move between
  adjacent pages. Execution does not automatically move a page or its scrollbar.
- **Close Debugger** clears the session's breakpoints and restores the normal CPU
  engine. Closing from the paused browser returns to the pause menu; closing the
  running control window continues gameplay.

Debugging uses the SH4 interpreter and can slow emulation. It does not change the
saved CPU-engine setting. Breakpoints compare instruction addresses without
patching game code. Resuming executes past the current stop once, allowing a
loop to hit that breakpoint again.

The browser shows memory disassembly, not execution history. Unmapped addresses
and device registers are shown as unavailable; inspecting them never performs
device reads or changes the guest TLB. Register values are inspected only while
execution is paused. The built-in debugger is unavailable during network play,
multiboard emulation, achievement hardcore mode, or while the GDB server is enabled.
