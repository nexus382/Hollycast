<!-- Portions Copyright 2026 The Hollycast Authors -->

# GDI and CUE game sizes

GDI and CUE entries now show the descriptor size plus the sizes of their referenced tracks. Shared track files count once, unrelated files in the folder are ignored, and missing or unreadable tracks keep the descriptor-size fallback.

[Source pull request #56](https://github.com/nexus382/Hollycast/pull/56)
