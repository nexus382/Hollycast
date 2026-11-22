<!-- Portions Copyright 2026 The Hollycast Authors -->
# Browser VMU core

The `VMS` JavaScript engine was extracted from
[bucanero/dcvmu-tool](https://github.com/bucanero/dcvmu-tool),
`web/index.html` at commit `fa9bf6d37ef8ebc25c25e4b3968df8428b94ac41`.
It is a port of SoftVMS 1.10 by Marcus Comstedt. The engine runs in the phone
browser, independently of Hollycast’s Dreamcast emulation.
Hollycast wraps the engine in `createVmu()` so two instances can keep independent
CPU, input, LCD and flash state in the same browser.

Licensed under GPL-3.0; see `COPYING`. The complete JavaScript source and license
are served alongside the phone page as `vmu-core.js` and `vmu-core.LICENSE`.
No Sega firmware or games are included. Optional user-supplied VMU firmware is
kept in the current browser tab.

The upstream engine is not cycle exact. Its firmware HLE supports mini-games;
the original file/game/clock menus require a user-supplied 64 KiB VMU BIOS.
