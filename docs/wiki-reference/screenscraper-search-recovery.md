<!-- Portions Copyright 2026 The Hollycast Authors -->

# ScreenScraper Short-Search Recovery

Searches shorter than three characters are skipped in favor of the full filename, avoiding the plain-text error ScreenScraper returned for short internal game names. A successful retry clears the stale error. Added error details omit credentials and request URLs.

[Source pull request #82](https://github.com/nexus382/Hollycast/pull/82)
