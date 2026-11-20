<!-- Portions Copyright 2026 The Hollycast Authors -->

# Cancel pending CD Rack launch when opening DCnet

Clears a pending CD Rack launch when the user switches to DCnet during the disc animation. Returning to Games leaves the library open instead of starting the game without another Play action. The two tabs share GUI state, so the tab transition needed to clear the launch explicitly.

## Check that switching tabs cancels Play

1. In a standalone build that includes the fix, select the **CD Rack** library layout and open **Games**. Have at least one launchable game in your library.
2. Select a game and choose **Play**. Switch to **DCnet** while the disc animation is still playing.
3. Return to **Games**. Switching to **DCnet** cancels the pending launch. Choose **Play** again and let the disc animation finish to start the game.

[Source pull request #48](https://github.com/nexus382/Hollycast/pull/48)
