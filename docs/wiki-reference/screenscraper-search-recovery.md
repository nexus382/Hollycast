<!-- Portions Copyright 2026 The Hollycast Authors -->

# ScreenScraper Short-Search Recovery

Searches shorter than three characters are skipped in favor of the full filename, avoiding the plain-text error ScreenScraper returned for short internal game names. A successful retry clears the stale error. Added error details omit credentials and request URLs.

## Retry a game with missing artwork

1. Open **Settings → Library → Artwork Downloads** and select **ScreenScraper**. Check that guest access or your personal account is available.
2. Confirm the game appears under a recognizable filename in your library. For affected short internal names, Hollycast uses the full filename for its next search.
3. Select **Retry Unmatched / Unavailable Media**. Use the ready and unavailable counts to see what the retry found, then return to a layout that shows the artwork you requested.

[Source pull request #82](https://github.com/nexus382/Hollycast/pull/82)
