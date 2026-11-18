<!-- Portions Copyright 2026 The Hollycast Authors -->

# Cancel pending CD Rack launch when opening DCnet

Clears a pending CD Rack launch when the user switches to DCnet during the disc animation. Returning to Games leaves the library open instead of starting the game without another Play action. The two tabs share GUI state, so the tab transition needed to clear the launch explicitly.

[Source pull request #48](https://github.com/nexus382/Hollycast/pull/48)
