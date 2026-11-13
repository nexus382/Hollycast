# Remote Play (phone)

Play on your phone while Hollycast runs on your computer. Video and game
audio stream to the phone browser, which acts as a Dreamcast controller.
Up to four phones can connect at once, one per controller port.

## Quick start

1. If `cloudflared` is missing, Hollycast offers **Install cloudflared** in
   the Remote Play settings: it downloads Cloudflare's official release into
   Hollycast's data folder (no administrator password, no account needed).
   A system install (cloudflare.com or `brew install cloudflared`) also works
   and is detected automatically.
2. In Hollycast, open **Settings → Network → Remote Play (Phone)** and choose
   **Start Remote Play**.
3. Open the `https://….trycloudflare.com` address shown there on your phone.
4. Enter the PIN shown in Hollycast and pick a controller (or "First
   available").

Start a game on the computer first (or after connecting — the stream follows
whatever Hollycast is showing). Touch controls: D-pad, analog stick, A/B/X/Y,
L/R triggers and Start. A Bluetooth controller paired to the phone also works.
When a phone connects, Hollycast automatically adds a Sega Controller to
that player's port if it was set to "None" (the default for Players 2-4),
so games can detect every phone.

## Notes and limits

- The tunnel address changes every time you start Remote Play. Anyone with
  the address still needs the PIN; five wrong PIN attempts lock logins from
  that network for a minute.
- macOS builds with WebRTC support stream H.264 video, stereo Opus game audio
  at 48 kHz and controller input over a direct connection. The latency badge
  shows the controller round-trip
  time and whether input uses WebRTC or the tunnel. This is network latency;
  video capture, decoding and the game's own processing add further delay.
- If WebRTC cannot connect, video falls back to motion JPEG and input uses
  the tunnel. This fallback has more delay and no phone audio. Other platforms
  currently use
  this fallback because their H.264 encoders are not implemented yet.
- OpenGL 3+ capture uses bounded asynchronous GPU readback, preserving the
  game's rendering state and dropping captures while the GPU is busy.
- Direct WebRTC may require a TURN relay on restrictive networks. Configure
  the TURN URL and credentials below if the connection repeatedly falls back.
- The phone's sound button mutes or enables its game audio. If its browser
  blocks audible autoplay, tap **Enable sound**. The computer keeps its own
  volume control; turning host volume down does not mute the phone.
- The host's Remote Play settings list all four players and their controller
  ports (A–D). Each row shows **Joining...**, **Connected**, or **Available**;
  the connected count updates as phone pages connect and close.
- Closing the phone page frees its controller slot. Stopping Remote Play (or
  quitting Hollycast) closes the tunnel. The tunnel process is supervised:
  if Hollycast crashes or is force-quit, `cloudflared` is killed with it
  instead of being left running.
- Without `cloudflared`, Remote Play still runs on
  `http://127.0.0.1:8765` on the host itself. Tick "Also allow connections on
  the local network" to reach it from phones on the same Wi-Fi without a
  tunnel (PIN still required).

## Settings (`[remoteplay]` in the config file)

| Key | Default | Meaning |
|---|---|---|
| `RemotePlayPort` | 8765 | Local server port |
| `RemotePlayFps` | 30 | Stream frame rate (5–60) |
| `RemotePlayWidth` | 640 | Stream width in pixels (160–1280) |
| `RemotePlayJpegQuality` | 70 | JPEG quality (30–95) |
| `RemotePlayBindLan` | false | Bind all interfaces instead of localhost |
| `RemotePlayAutoTunnel` | true | Open the Cloudflare tunnel on start |
| `RemotePlayAutoStart` | false | Start Remote Play when Hollycast launches |
| `RemotePlayPin` | (random) | Fixed PIN; empty means a new PIN each start |
| `RemotePlayCloudflaredPath` | (auto) | Explicit path to the `cloudflared` binary |
| `RemotePlayBitrateKbps` | 1500 | H.264 target bitrate in kilobits per second |
| `RemotePlayTurnUrl` | (empty) | Optional TURN relay URL |
| `RemotePlayTurnUsername` | (empty) | TURN username |
| `RemotePlayTurnPassword` | (empty) | TURN password |

## Developer checks

`RemotePlayCaptureTest.*` opens a hidden OpenGL context and verifies that
repeated captures preserve rendering state and return the expected pixels.
`RemotePlayTest.*` covers input, controller slots, authentication helpers,
and real WebRTC negotiation, video, audio and direct controller messages.
`RemotePlayAudioTest.*` decodes encoded stereo test tones and checks sample
rate and channel order across capture block boundaries.
`node tests/remoteplay/client_heartbeat_test.mjs` verifies that signaling
stays active while input uses WebRTC, and timers are cleared on disconnect.
`node tests/remoteplay/client_audio_test.mjs` checks combined media tracks,
audible autoplay recovery and the sound control.

For independent H.264 and Opus decoding and a direct-input round-trip measurement, run
`tests/remoteplay/webrtc_smoke.py` with `aiortc` and `websockets` installed,
`REMOTEPLAY_PIN` set to the host's PIN, and `--url` set to the Remote Play
address. Start a game first. The script releases its controller when done
and does not print authentication tokens or signaling credentials. The check
requires non-silent stereo game audio, so leave the game running.
Enter gameplay if the attract sequence is silent.
