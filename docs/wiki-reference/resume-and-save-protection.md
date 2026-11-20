<!-- Portions Copyright 2026 The Hollycast Authors -->

# Fix Continue Playing scans and Resume startup

Follow-up fixes for Continue Playing: save checks run in the background, and Resume uses the selected game's settings. Network modes that cannot resume a state show a message. Failed Resume attempts preserve existing saves.

## Resume a game safely

1. In an offline title, use **Save State** in the pause menu or enable **Auto-Save on Exit** under **Settings → General → Automatic Save States**.
2. Leave the game and open **Continue Playing**. Find the title and choose **Resume** to load its latest manual state or autosave.
3. Choose **Start Fresh** when you want to boot from the beginning. If a network or other restricted session refuses Resume, keep the save and return to an eligible offline session to load it.

[Source pull request #33](https://github.com/nexus382/Hollycast/pull/33)
