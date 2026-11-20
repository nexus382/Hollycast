<!-- Portions Copyright 2026 The Hollycast Authors -->

# Phone Remote Play

Phone Remote Play is under review. The draft would let up to four players open the host game in a phone browser and use their phones as controllers. The draft includes WebRTC video and audio paths for macOS and Windows.

## Try the draft build

1. On a desktop host running a build from PR #71, open **Settings → Network → Remote Play (Phone)** and select **Start Remote Play**. A phone-accessible address requires the Cloudflare tunnel in the draft setup.
2. When **On your phone** shows an address, open it in your phone browser and enter the host's **PIN**.
3. Start a game and try the phone controls. Look for **Connected** and the assigned controller port in the host player list. If the browser prompts for audio, select **Enable sound**.
4. Select **Stop Remote Play** on the host to end the session.

[Source pull request #71](https://github.com/nexus382/Hollycast/pull/71)
