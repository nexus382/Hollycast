<!-- Portions Copyright 2026 The Hollycast Authors -->

# Android and PC Split-Screen Netplay

Android–PC split-screen netplay is in development. The draft uses GGPO for two-player sessions between Android and PC. Reconnect and in-game desync are still open problems.

## Check the draft on Android and PC

1. Use matching builds from the draft in PR #73 on Android and PC.
2. Use the same dump of a game with local split-screen multiplayer on both devices. Back up the host's VMU files before starting a session.
3. Host a two-player GGPO session on one device and join from the other. Start a match and play from both devices.
4. If the session disconnects or the screens fall out of sync, see the linked PR for known issues and what to include in a report.

[Source pull request #73](https://github.com/nexus382/Hollycast/pull/73)
