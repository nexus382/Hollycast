# Remote Play integration decision

**Status: proposed — maintainer decides**

Review date: 2026-11-27. The review covers local source and Git history.

## Recommendation

**Park the branch for now.** Remote Play offers a useful way to play from a phone and has a substantial implementation, but it adds an inbound HTTP/WebSocket service, a public tunnel option, a custom authentication/session layer, and a downloaded executable before the integration and denial-of-service boundaries have been established. The listener and tunnel are not enabled automatically by default (`RemotePlayAutoStart` is false), but the feature is compiled into standalone builds and can be started from settings; on start, the default `RemotePlayAutoTunnel` opens the tunnel. A default-off build option plus explicit runtime opt-in should be the target if it is reconsidered.

- **User value:** The branch supports up to four phones as controllers, with MJPEG fallback and H.264/Opus/WebRTC on supported macOS builds. Integration and current-base runtime validation remain pending.
- **Platform statement:** `README.md:76` says Hollycast aims to support every platform and release that Flycast supports. The branch provides WebRTC H.264/Opus only when built for macOS; other builds use MJPEG and have no phone audio. A future landing needs an explicit experimental, per-platform support statement rather than implying parity. See `core/ui/CMakeLists.txt:126-178` and `docs/remote-play.md:32-40` on the branch.
- **Security gates:** Resolve the request and fragmented-message memory bounds, establish an artifact-verification policy for the in-app `cloudflared` installer, and review session confidentiality on plain HTTP LAN connections before enabling the feature broadly. Details follow.
- **Maintenance/rebase:** The branch tip is one feature commit after its common base, but the target has advanced substantially and shares many files with it. A current-base build and conflict review remain outstanding.

The branch should remain discoverable by its existing name and tip while parked. No branch tagging or cleanup is requested here. If it is later reconsidered, first close every security gate below, then repeat a current-base rebase/build review and make the experimental platform statement explicit. This note does not approve a merge or enablement.

## Branch inventory

Drift check: `feat/remote-play-streaming` is `e685284da` (`Add phone Remote Play with stereo audio and live player status`, 2026-11-13), matching the planned tip. `git diff --stat master...feat/remote-play-streaming` reports 45 files, 4,456 insertions, and 12 deletions. The branch has one commit after merge-base `df47e99c3` and is 198 commits behind local `dev` by commit reachability (`git rev-list --left-right --count dev...feat/remote-play-streaming`: `198 1`).

### Added paths (28)

- `core/deps/libdatachannel`, `core/deps/opus`: new submodule pins for WebRTC transport and Opus audio.
- `core/input/remote_gamepad.cpp`, `core/input/remote_gamepad.h`: adapt validated phone button, trigger, and stick events to Hollycast gamepad input.
- `core/rend/gles/frame_readback.h`: bounded asynchronous GLES frame readback support.
- `core/ui/remoteplay/audio_stream.cpp`, `core/ui/remoteplay/audio_stream.h`: capture and distribute host game audio to remote peers.
- `core/ui/remoteplay/cloudflare_tunnel.cpp`, `core/ui/remoteplay/cloudflare_tunnel.h`: locate, start, supervise, stop, and optionally install `cloudflared`.
- `core/ui/remoteplay/h264_encoder.h`, `core/ui/remoteplay/h264_encoder_stub.cpp`, `core/ui/remoteplay/h264_encoder_vt.mm`: encoder interface, unsupported-platform stub, and VideoToolbox H.264 implementation.
- `core/ui/remoteplay/opus_encoder.cpp`, `core/ui/remoteplay/opus_encoder.h`: Opus audio encoder.
- `core/ui/remoteplay/remote_play.cpp`, `core/ui/remoteplay/remote_play.h`: HTTP/WebSocket listener, authentication, sessions, capture, stream, and server lifecycle.
- `core/ui/remoteplay/remote_play_page.h`: embedded phone browser UI and touch/gamepad client.
- `core/ui/remoteplay/remote_play_protocol.h`: HTTP/WebSocket helpers, PIN comparison, player slots, and remote input parsing.
- `core/ui/remoteplay/webrtc_peer.cpp`, `core/ui/remoteplay/webrtc_peer.h`: WebRTC peer negotiation, media tracks, and data-channel input.
- `docs/remote-play.md`: user and developer guide.
- `tests/remoteplay/client_audio_test.mjs`, `tests/remoteplay/client_heartbeat_test.mjs`, `tests/remoteplay/webrtc_smoke.py`: browser audio/heartbeat checks and an independent WebRTC smoke client.
- `tests/src/ui/RemotePlayAudioTest.cpp`, `tests/src/ui/RemotePlayCaptureTest.cpp`, `tests/src/ui/RemotePlayTest.cpp`, `tests/src/ui/RemotePlayWebRtcTest.cpp`: audio, renderer capture, protocol/input/session, and WebRTC tests.

### Modified existing paths (17)

- `.gitmodules`: registers the two new dependencies.
- `core/audio/audiostream.cpp`, `core/audio/audiostream.h`: add the audio capture path used by remote streaming.
- `core/cfg/option.cpp`, `core/cfg/option.h`: add Remote Play port, stream, tunnel, PIN, and TURN options.
- `core/hw/pvr/Renderer_if.h`: add the renderer capture interface.
- `core/input/CMakeLists.txt`: compile the remote gamepad adapter.
- `core/rend/gles/gldraw.cpp`, `core/rend/gles/gles.cpp`, `core/rend/gles/gles.h`: connect GLES frame capture and readback.
- `core/ui/CMakeLists.txt`: compile the listener and select optional macOS WebRTC/encoder dependencies.
- `core/ui/gui.cpp`, `core/ui/gui.h`, `core/ui/mainui.cpp`: connect frame capture, startup, and shutdown to the UI lifecycle, and redraw the last game image under OSD-only frames.
- `core/ui/settings_new.cpp`: expose manual start/stop, PIN, tunnel, player state, and settings controls.
- `tests/CMakeLists.txt`: register Remote Play tests.
- `tests/src/AssemblyDebuggerTest.cpp`: add regression coverage for redrawing the last game image under OSD controls when a game frame is unavailable; no Remote Play runtime behavior is added here.

## Current gating and platform behavior

The standalone CMake path includes the UI and server sources without a Remote Play-wide compile option. `HOLLYCAST_REMOTEPLAY_WEBRTC` is a separate option, defaulting to `ON`, and only the `APPLE AND NOT IOS` build path wires WebRTC, Opus, and VideoToolbox (`core/ui/CMakeLists.txt:34-35,126-178`). Other builds compile an H.264 stub and use MJPEG. `RemotePlayAutoStart` defaults to `false`; `RemotePlayAutoTunnel` defaults to `true` (`core/cfg/option.cpp:261-273`). Starting is a manual settings action, but it starts the tunnel automatically when that option is left enabled (`core/ui/settings_new.cpp:9146-9168`).

The HTTP service defaults to `127.0.0.1:8765`; enabling `RemotePlayBindLan` changes the bind to all IPv4 interfaces (`remote_play.cpp:679-720`). When enabled, the Cloudflare tunnel targets the loopback listener (`cloudflare_tunnel.cpp:157-175,191-192,238-252`). The user guide describes HTTPS `trycloudflare.com` access and the HTTP loopback/LAN alternatives (`docs/remote-play.md:14-18,55-58`).

## Static security review

This is defensive source review only. No server, tunnel, or phone client was run; no attack traffic was sent. Line references below are to `feat/remote-play-streaming` at `e685284da`.

### Network listener and authentication

- `remote_play.cpp:679-720` creates one TCP listener on the configured port. It binds loopback by default and binds `INADDR_ANY` when the LAN option is enabled. The backlog is 16. Each accepted connection starts a detached handler thread (`:636-656`).
- `remote_play.cpp:121-127,236-288` generates a six-digit PIN from `std::random_device` unless a fixed config PIN is set. A constant-time comparison is used (`remote_play_protocol.h:219-233`). Five wrong PINs block that client IP for one minute (`remote_play.cpp:236-268`). There is no attempt cap across source IPs, and the per-IP guard map has no observed expiry/pruning path. The cryptographic quality of `std::random_device` is implementation-defined, so the PIN's effective entropy is not established by this review.
- Successful login creates a 32-hex-character session token and an `HttpOnly; SameSite=Strict` cookie (`remote_play.cpp:278-288`). The cookie lacks `Secure`, which is relevant because the optional LAN listener and local URL use plain HTTP. The public tunnel URL is HTTPS at the client-facing end; the source reviewed here does not establish the complete confidentiality properties across the tunnel provider.
- `/api/status`, `/stream.mjpg`, and `/ws` require that session cookie (`remote_play.cpp:589-627`). Controller input is limited by the protocol parser to buttons, triggers, stick, release, ping, and a menu button (`remote_play_protocol.h:301-425`); accepted input feeds a `RemoteGamepad` (`remote_play.cpp:381-440`). The reviewed path is game/controller input, not arbitrary host keyboard, file, or shell access.

### Resource bounds and protocol handling — must fix before landing

- `readRequest` caps header accumulation at 65,536 bytes but trusts `Content-Length` and reads the body until that length (`remote_play.cpp:151-206`). No body-size limit or request read timeout is visible. Together with one detached thread per accepted socket (`:636-656`), unauthenticated clients can hold threads or request arbitrarily large allocations. Add strict body/connection limits and bounded handler concurrency before exposure.
- WebSocket frames are individually capped at 16 MiB (`remote_play_protocol.h:184-216`), but fragmented payloads accumulate in `message` (`remote_play.cpp:399-444`). The apparent 4 KiB guard is after `message.clear()` at `:441-443`, so it cannot limit a fragmented aggregate. Enforce a real aggregate limit and reject invalid fragmentation/control-frame sequences before landing.
- The PIN is six digits, and IP-based throttling alone is a weak control for a public tunnel with many possible source addresses. Reassess online guessing controls, source aggregation, and how long-lived fixed PIN configuration is handled before public exposure.

### Tunnel process and binary installation — must fix before landing

- `tunnelStart` invokes the discovered executable with `tunnel --url http://127.0.0.1:<port> --no-autoupdate` (`cloudflare_tunnel.cpp:157-192,219-298`). On POSIX a supervisor watches Hollycast and terminates `cloudflared` after host exit; normal stop signals the supervisor (`:238-298,301-339`). On Windows a kill-on-close job is attempted, but the return values from job creation/configuration/assignment are not checked (`:203-212`); failure can weaken the promised crash cleanup. Check and handle these failures.
- The in-app installer downloads a platform artifact from a moving GitHub `releases/latest/download` URL, checks only HTTP success and a minimum byte count, and does not verify a pinned digest or signature (`cloudflare_tunnel.cpp:359-380,407-466`). On macOS it writes the archive and invokes `tar -xzf` through `std::system` (`:434-449`). Define artifact authenticity and safe extraction guarantees before shipping the downloader; otherwise require a separately installed, trusted binary.
- The public tunnel makes the host listener reachable through a third-party service. Its ephemeral URL and PIN reduce casual discovery but do not replace resource limits, transport/session review, or clear user consent and platform support wording.

### Comparison with Phone VMU

Phone VMU binds `0.0.0.0` on its local port and uses a persistent pairing token in the URL (`core/network/phone_vmu.cpp:436-447`). Its documented reach is a phone browser on the local network; anyone holding the URL can view the LCD and access published card copies, and a card session can sync card changes. It does not give phone buttons control of the Dreamcast game, and the docs say not to forward its HTTP port (`docs/Phone VMU.md:173-177`). Remote Play adds real-time game/controller input and an optional internet-facing tunnel, so it has a broader exposure and should not inherit Phone VMU's current assumptions as sufficient controls.

### Other review limits

The source review did not exercise the parser, authenticate a client, inspect runtime socket behavior, verify the Cloudflare release chain, or assess transitive dependency advisories. Those remain for a later authorized security and integration review.

## Rebase/build feasibility evidence

The feature and local `dev` share base `df47e99c3`. At review time, `dev` had 198 commits beyond that base and the feature had one unique commit. Comparing both the feature and audit commit `ea33143d8` against their common base finds 15 paths modified on both sides, including audio, configuration, input, renderer and UI wiring, settings, and test registration. Rebase conflicts and current-base build compatibility remain unverified.

The branch adds four Google Test files (Remote Play protocol/session, WebRTC, capture, and audio), two Node browser checks, and a Python WebRTC smoke script. Their presence is inventory evidence only; they were not run in this decision task.

## CODEMAP coordination

The development checkout has a local, Git-excluded code map. Keep its shipped subsystem inventory aligned with the current checkout. This decision note records the branch-only Remote Play implementation.
