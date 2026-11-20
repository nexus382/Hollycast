<!-- Portions Copyright 2026 The Hollycast Authors -->

# Large library browsing

Library filters, artwork refreshes, and save-state discovery now update in batches or in the background. Applying a video preset resets its texture-filtering values so older custom values do not linger. LaunchBox keeps media already downloaded when a request fails.

## Check browsing and artwork refresh

1. Add a folder with several games under **Settings → General → Content Paths**, then return to the library.
2. With an internet connection, choose a provider under **Settings → Library → Artwork Downloads → Artwork Provider**, then choose media in **Images to Download** and select **Download Selected Media**.
3. Keep browsing or change your library filters while artwork downloads in the background.

[Source pull request #52](https://github.com/nexus382/Hollycast/pull/52)
