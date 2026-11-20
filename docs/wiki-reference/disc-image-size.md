<!-- Portions Copyright 2026 The Hollycast Authors -->

# GDI and CUE game sizes

GDI and CUE entries now show the descriptor size plus the sizes of their referenced tracks. Shared track files count once, unrelated files in the folder are ignored, and missing or unreadable tracks keep the descriptor-size fallback.

## Check a multi-track game's size

1. Add a folder containing a complete GDI or CUE dump. Keep the descriptor and every track it references in the paths specified by the descriptor.
2. Select the **App UI Theme** under **Settings → Library → Library Themes**, then set **Library Display Style** to **List**. Find the entry in **All Games** and check its **Size** column.
3. The **Size** column counts the descriptor and each distinct track it references.

[Source pull request #56](https://github.com/nexus382/Hollycast/pull/56)
