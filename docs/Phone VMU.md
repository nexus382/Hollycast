<!-- Copyright 2026 The Hollycast Authors -->
# Use your phone as a VMU

Standalone Hollycast on Windows, macOS and Linux can show the Dreamcast VMU LCD
and play standalone VMU mini-games in a phone browser. No phone app or internet
connection is required.

1. Connect your computer and phone to the same local network. Ethernet on the
   computer and Wi-Fi on the phone work together.
2. Open **Settings > On-screen display > Phone VMU** in Hollycast and enable
   **Connect Phone**.
3. Scan the QR code with the phone camera, or open the shown `http://` address.
4. Choose a slot: A1/A2 belong to controller A, B1/B2 to B, and so on.
5. Close settings and play. The phone mirrors the selected LCD independently of
   **Show VMU In-Game** and **Move VMU**. Use your usual controller for the
   Dreamcast game.

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
card to the phone. The mini-game runs in the browser on its own VMU processor.
The phone must remain awake with the browser visible.

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
- **Dreamcast display** returns to the live LCD stream. You can then change slots.
- Keyboard controls: arrows for the D-pad, A/Z for A, B/X for B, M/Enter for Mode,
  and S/Space for Sleep. Focused VMU buttons also work with Enter or Space.

The browser uses a copy. **Download VMU copy** exports a 128 KiB card image with
any changes the mini-game made. Download it before closing or reloading the tab
to keep progress. Changes are not automatically sent back to Hollycast or merged
into the active memory card. To use an exported card in Hollycast, transfer it to
the computer and select it through Hollycast's memory-card settings.

The core is a JavaScript port of SoftVMS 1.10. It is not cycle exact, so game
compatibility varies. Firmware calls used by many mini-games work without a
BIOS; the original file/game/clock menus and some games require your own VMU BIOS.
Use **Load VMU BIOS** for a 64 KiB firmware dump. It is kept in the current tab.
No Sega BIOS or games are bundled. Credits, complete core source and GPLv3 license
are available through **Credits and license** on the phone.

## Ending sharing

Turn off **Connect Phone** or exit Hollycast to end live network access. Sharing
starts disabled each time Hollycast launches. Enabling it again creates a new
address and pairing code; rescan instead of reusing the old address. A mini-game
already loaded on the phone can continue locally after sharing stops.

Anyone with the pairing address on a reachable network can view the LCD and
obtain copies of published VMU cards until sharing stops. The server is read-only:
phone input and modified cards are never sent back to the computer. It uses local
HTTP on an automatically selected TCP port; do not forward that port to the internet.

## Connection problems

- If multiple addresses appear, choose the Wi-Fi or Ethernet address; VPN and
  virtual adapters have their own addresses.
- Allow Hollycast through the firewall on private networks and allow local
  network access on macOS when prompted.
- Guest Wi-Fi, client isolation and some VPNs prevent local device connections.
- Background tabs stop LCD requests and pause mini-game execution. Lost touches
  or keyboard focus release held buttons. A lost LCD connection retries automatically.
- After changing networks, turn sharing off and back on to refresh the addresses.

Hosting is not offered by the libretro core, mobile, Switch or UWP builds. Phones
with a browser can still connect to supported desktop hosts.
