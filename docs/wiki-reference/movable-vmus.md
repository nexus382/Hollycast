<!-- Portions Copyright 2026 The Hollycast Authors -->

# Add movable VMUs on Windows, Android, macOS and Linux

Adds movable VMU screens with a persistent position lock, reset control and size setting independent of UI scaling. Desktop uses separate windows; Android draws the screens in-app and adapts positions to rotation and split-screen. If a desktop VMU window cannot open or render, its LCD returns to the game view. Also fixes the FreeBSD CMake package name.

## Move and lock a VMU display

1. Start a Dreamcast game with a VMU attached. In **On Screen Display → In-Game Display**, turn on **Show VMU In-Game** and **Move VMU**.
2. Drag the VMU display to the desired position, then turn off **Move VMU** to lock it. Return to the game and check that the screen stays where you placed it.
3. Use **VMU Size** to adjust the display or the reset icon beside **Move VMU** to restore its default position.

[Source pull request #15](https://github.com/nexus382/Hollycast/pull/15)
