<!-- Portions Copyright 2026 The Hollycast Authors -->

# Texture reconstruction filters with live tuning and renderer fixes

Choose a filter in the video settings. Set a maximum texture width to limit where it applies. Use **Texture Filtering Live Tune** to see changes during play. The new filters work with Vulkan, Direct3D 11, OpenGL 3 and OpenGL ES 3.

## Steps

1. On a supported renderer, open **Settings > Video** and choose a **Sample Texture Filter**.
2. Set **Max Filtered Texture Size** to the largest texture width you want the filter to affect.
3. Start a game offline, return to **Settings > Video**, and open **Texture Filtering Live Tune**. Adjust the sample filter and compare the same edges or textures in the running game.

[Source pull request #5](https://github.com/nexus382/Hollycast/pull/5)
