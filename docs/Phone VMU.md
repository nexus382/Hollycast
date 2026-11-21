<!-- Copyright 2026 The Hollycast Authors -->
<!-- Portions Copyright 2026 The Hollycast Authors -->
# Use your phone as a VMU

Standalone Hollycast on Windows, macOS, Linux and Android can share the Dreamcast
VMU LCD and standalone VMU mini-games with another device's browser. No client
app or internet connection is required.

1. Connect the device running Hollycast and the device running the browser to
   the same local network. Ethernet and Wi-Fi work together.
2. Open **Settings > On-screen display > Phone VMU** in Hollycast and enable
   **Connect Phone**.
3. On the other device, scan the QR code or open a slot address from Hollycast.
   **COPY A1 URL** and **COPY A2 URL** copy addresses that open the selected card
   directly.
4. If you scanned the QR code, choose a slot in the browser: A1/A2 belong to
   controller A, B1/B2 to B, and so on.
5. Close settings and play. The phone mirrors the selected LCD independently of
   **Show VMU In-Game** and **Move VMU**. Use your usual controller for the
   Dreamcast game.

When hosting on Android, keep Hollycast in the foreground and open the pairing
page on another device, such as a PC on the same Wi-Fi network.

The page itself is the VMU: LCD, D-pad, A/B, Mode and Sleep. Tap the LCD to reveal
settings. **LCD only** fills the whole page with just the LCD; **Show buttons**
restores the full VMU. Rotation and browser fullscreen are also available.
The settings overlay hides automatically or with **Hide controls**.
Double-tapping does not zoom the page; pinch-to-zoom remains available.

**Keep awake** is on by default. Tap the VMU once after opening it to prevent
automatic screen sleep while the page is visible. This works for the live display
and standalone mini-games. The controls show whether keeping awake succeeded;
turn **Keep awake** off to allow the phone to sleep normally. VMU **Sleep**, leaving
the page, or switching apps releases it. Returning to the page tries again; tap
once if the browser asks for another interaction.

Where available, the page uses the browser's Screen Wake Lock API. Local `http://`
pairing links use a small silent inline video from NoSleep.js instead, since the
native API requires a secure context. Neither method needs internet access. A
browser or battery-saving setting may still refuse to keep the screen on; if so,
adjust the phone's Auto-Lock/display-timeout setting while playing.

## Standalone mini-games

Wait until the Dreamcast has finished saving, then press **Mode** or choose
**Play mini-game on this card**. Hollycast sends a snapshot of that slot's VMU
card to the phone and temporarily unplugs that card from the virtual controller.
The mini-game runs in the browser on its own VMU processor. Progress saved by the
mini-game syncs back to Hollycast automatically while connected. The phone must
remain awake with the browser visible.

Alternatively, use **Open VMU game** to open a `.vms` mini-game, or a raw 128 KiB
`.bin`/`.vmu` card containing one, from your phone. The chosen file stays on the
phone. These formats do not include standalone Dreamcast save files without a
VMU game. Nexus `.dci`/`.dcm` files must be converted to a supported format first.

- **D-pad and A/B** operate the mini-game. Multi-touch supports holding A+B,
  which many games use to start. These buttons do not control the Dreamcast.
- **Mode**, while a mini-game is running, sends its VMU Mode input. Some games
  exit when pressed. Use **Restart mini-game** to play again.
- **Sleep** pauses the mini-game and blanks the LCD; press it again to resume.
- **Sound on** enables the VMU buzzer in browsers supporting Web Audio.
- **Return to Dreamcast** saves the latest progress, reconnects the VMU card and
  returns to the live LCD stream. You can then bring the character back using the
  Dreamcast game's normal VMU transfer option. No files need to be moved. This
  does not bypass the game's own transfer steps or guarantee minigame compatibility.
- Keyboard controls: arrows for the D-pad, A/Z for A, B/X for B, M/Enter for Mode,
  and S/Space for Sleep. Focused VMU buttons also work with Enter or Space.

If a card changes in Hollycast and on the phone at the same time, sync stops and
asks which copy to keep: **Keep my phone copy** replaces the Hollycast card (a
backup snapshot is saved first), **Use Hollycast copy** discards the phone's
changes so you can reopen the card from Hollycast. Speed, sound and FF are
remembered per game; the VMU BIOS is remembered in this browser across games.
**Card files** lists the open card's files with its free blocks and can export,
import or delete an individual file; changes sync like game progress.

The sync status confirms when progress has reached Hollycast's memory-card file.
If the Dreamcast is paused, resume it so queued updates can finish. Connection
failures retry automatically. The current phone copy and unfinished sync are kept
in browser session storage when available, allowing the same tab to recover after
a reload. Keep the tab open until **Return to Dreamcast** confirms the save.

Only one phone can use a card at a time, and the Dreamcast cannot access that card
while it is on the phone. Before the first update of each phone session, Hollycast
saves the original card beside its VMU file as a timestamped
`.phone-backup-YYYYMMDD-HHMMSS` snapshot, keeping the newest five per card. Stop
emulation and use **Restore VMU … phone backup** in the Phone VMU settings to
roll a card back to its newest snapshot. Loading
a save state, replacing the card, entering netplay or stopping sharing invalidates
the phone's ownership. Sync then stops instead of overwriting desktop changes.
Read-only cards and netplay do not support automatic sync.

### Experimental browser-to-browser VMU link

1. Use **COPY A1 URL** and **COPY A2 URL** to open the two card pages before
   starting either mini-game. Use two devices or separate visible browser
   windows, such as split screen on a phone. Both pages must use the same
   Hollycast host.
2. Start each mini-game. If the game asks you to connect VMUs, reach that prompt
   on both screens first.
3. In A1, tap the LCD to open **Controls**, then choose **Create VMU link**. In
   A2, choose **Join A1**. To retry a link, disconnect and repeat these two
   actions in the same pages. The card addresses stay the same.

To link other slots, open the general pairing page and choose the two cards.
Use **Create VMU link** on one side and open its invite QR or URL in the other
browser, then choose **Join VMU link**. The invite expires after two minutes.
Tap the LCD to show the link status, recall a hidden invite, or disconnect.

The linked VMUs advance together at normal speed. Each VMU stays in its own
browser and continues to sync to its own slot. In Chao Adventure, wait until both
screens say **Please Disconnect**, then choose **Disconnect VMU link** to unplug
the cable and continue in the mini-game. For a Hollycast card, choose
**Return to Dreamcast** in its browser to save and reinsert it. For a game you
opened from a file, choose **Download VMU copy** before **Close local game** to
keep the result. Create a fresh invite for the next link.

Games opened through **Open VMU game** stay local and are never uploaded.
**Download VMU copy** remains available to export a 128 KiB card image for local
games or as a fallback after a sync conflict. Browser storage may be cleared by
the phone or unavailable in private browsing; download the copy if syncing cannot
finish. To use an exported card in Hollycast, transfer it to the computer and
select it through Hollycast's memory-card settings.

The core is a JavaScript port of SoftVMS 1.10. It is not cycle exact, so game
compatibility varies. Firmware calls used by many mini-games work without a
BIOS; the original file/game/clock menus and some games require your own VMU BIOS.
Use **Load VMU BIOS** for a 64 KiB firmware dump. It is remembered in this
browser's storage across page reloads and games.
No Sega BIOS or games are bundled. Credits, complete core source and GPLv3 license
are available through **Credits and license** on the phone.

## Mate two adult Chao and bring both eggs back

Use a Hollycast build that shows **COPY A1 URL** and **COPY A2 URL** in Phone VMU
settings. The host and browsers should be on the same local network.

1. In *Sonic Adventure*, use the Chao Transporter to put one **adult** Chao on each
   of two different VMUs, such as **A1** and **A2**.
2. Open the A1 and A2 addresses in separate visible browser panes and choose
   **Play mini-game on this card** on each. Press **A+B** to start
   *Chao Adventure*. In both games, open **Mating** and stop at **Please Connect**.
3. Choose **Create VMU link** in A1, then **Join A1** in A2.
4. Wait until **both** screens say **Please Disconnect**, then choose
   **Disconnect VMU link** on either device. Let both animations finish.
   Press **A** on each VMU to return to the menu, then press **Mode** and choose
   **Return to Dreamcast** in each browser. Wait for both saves to finish syncing.
5. In the garden, jump onto the Transporter's red button with empty hands.
   Bring in the egg from the first VMU, then repeat for the second.
   Select just the egg to leave its parent on the VMU.

Each VMU gets its own egg. You can bring both eggs into the same garden.

## Ending sharing

Choose **Disconnect VMU link** in either browser to unplug two linked VMUs.
An established browser link can keep running after Hollycast closes. Turn off
**Connect Phone** or exit Hollycast to stop serving LCD frames and cards. Sharing
starts disabled each time Hollycast launches, but the pairing address stays the
same across restarts: reopen the saved page to reconnect. Its QR code remains
valid if you need it again. Use **New pairing address** in Phone VMU settings to disconnect
all paired phones and issue a fresh code. A mini-game already loaded on the
phone can continue locally after sharing stops. Return the card before stopping
sharing to keep the latest progress in Hollycast. Unsynced phone progress is
retained locally when browser session storage is available.

Reload recovery applies to the same pairing page. A new pairing address starts a
separate session; return the VMU before regenerating it, or download its copy
from the old phone tab first.

Anyone with the pairing address on a reachable network can view the LCD and
obtain copies of published VMU cards until sharing stops. Opening a card's minigame
also permits that phone to sync progress to the card while it owns it. Phone
buttons do not control the Dreamcast game. The service uses local
HTTP on a fixed TCP port (28743 by default); do not forward that port to the internet.

## Connection problems

- If multiple addresses appear, choose the Wi-Fi or Ethernet address; VPN and
  virtual adapters have their own addresses.
- Allow Hollycast through the firewall on private networks and allow local
  network access on macOS when prompted.
- Guest Wi-Fi, client isolation and some VPNs prevent local device connections.
- Background tabs stop LCD requests and pause solo mini-games. A linked tab
  keeps its VMU clock running while the browser allows it. If your phone
  suspends a tab and the link drops, bring both tabs back and create a fresh
  invite. Lost touches or keyboard focus release held buttons.
- After changing networks, turn sharing off and back on to refresh the local
  addresses. Use the updated address shown in Hollycast; scan its QR code if the
  saved URL points to the previous network.
- If a VMU is already in use, return it from the phone holding it. If that tab
  was lost, turn **Connect Phone** off and on, then reload the saved pairing page.
  Its address remains valid; choose a copy if Hollycast reports a card conflict.

Hosting is not offered by the libretro core or by iOS, Switch or UWP builds.
Devices with a browser can still connect to a supported host.
