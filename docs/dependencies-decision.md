# Dependency ownership and replacement decisions

**Status: proposed** — recommendations for operator review; no fork, pin, or library change is enacted here. Prepared 2026-11-27 from this checkout.

This review compares the checked-out pins and proposes a next step for four dependencies. The public repository pages were accessible in a browser. The worktree's `git ls-remote` checks failed at DNS resolution, so current remote heads and release status remain unverified. No `.gitmodules` entry has a `branch =` setting. [WebSocket++](https://github.com/zaphoyd/websocketpp) · [Discord RPC fork](https://github.com/harmonytf/discord-rpc) · [MinGW Breakpad fork](https://github.com/flyinghead/mingw-breakpad) · [LuaBridge](https://github.com/vinniefalco/LuaBridge)

## websocketpp

### Context

- `.gitmodules:55-57` points `core/deps/websocketpp` to `https://github.com/zaphoyd/websocketpp.git`. The checked-out pin is `1b11fd301531e6df35a6107c1e8665b1e77a2d8e`, dated 2020-04-27 by `git -C core/deps/websocketpp log -1 --format='%H %ad %s' --date=iso-strict`; the in-tree description is `0.8.2-1-g1b11fd3`. Superproject history (`git log -- core/deps/websocketpp`) records the submodule being added at `63534c5d5` (2025-04-07). No later superproject change to this path is in the retained history.
- The consumer is `core/network/ice.cpp`. It includes the Asio no-TLS client and client endpoint (`:81-82`), derives `CustomWsppConfig` from `asio_client` (`:120-127`), runs the Asio event loop on `asioThread` (`:280-282`), connects to `ws://lobby.flyca.st/<room>` (`:283-286`), registers open/fail/close/message callbacks (`:290-337`), and sends text frames (`:354-359`). `onWsReceive` dispatches lobby commands including `join`, `userlist`, `candidate`, `leave`, `challenge`, `chalresp`, and `say` (`:362-455`). Candidate exchange calls `juice_set_remote_description` (`:395-398`); game data uses `juice_send` (`:633`). Thus the socket provides lobby/chat/challenge/signaling control, while peer data transport is separate.
- The custom configuration sets `enable_multithreading = false`, a 4 KiB connection read buffer, and 2 KiB message and HTTP body limits (`ice.cpp:120-126`). The event loop is run on a dedicated thread, while the session's methods can send from their callers. The code has no comments explaining the configuration choices, so buffer bounding is an inference, not recorded rationale. The active endpoint is plain `ws://`; TLS is not configured.
- Inspection of the vendored dependency directories found no alternative WebSocket client already in-tree. Asio is present as a submodule, but it is a transport library, not a WebSocket implementation. `core/deps/README.md` records the dependency inventory.
- `git -C core/deps/websocketpp ls-remote origin HEAD` failed with `Could not resolve host: github.com`; upstream liveness and any later release could not be checked.

### Options

**A. Adopt and own the current implementation.** Create an organization-owned fork/mirror at the operator's chosen URL, keep the present pin initially, assign a maintainer to watch upstream advisories and CVEs, and define a patch/release policy for security fixes and tested upgrades. **Effort: M. Risk: Medium**, because this creates ongoing security and maintenance duties on a lobby-facing parser.

**B. Replace it.** Boost.Beast is an external candidate to evaluate because Asio is already pinned; it is not present in-tree. A replacement would touch `core/network/ice.cpp`, build/dependency wiring, callback and event-loop integration, and teardown/send behavior. **Effort: L. Risk: High**, because framing, room protocol, connection lifecycle, and cross-thread behavior must remain compatible with the live lobby service. The gate must include real-peer netplay/lobby interoperability tests; no such evidence is part of this spike.

No upstream-bump option is recommended now: the remote could not be reached, so no newer live release or commit was verified.

### Recommendation

Propose adopting ownership of the current WebSocket++ dependency and creating an organization fork before the next dependency change. Defer any replacement to a separate implementation plan with real lobby and peer interoperability as its acceptance gate; the operator must supply the fork URL before `.gitmodules` can be changed.

## discord-rpc

### Context

- `.gitmodules:34-36` points to `https://github.com/harmonytf/discord-rpc`. The pin is `c62c34d37cd90b75187a1212e13ce4145bafbdd7`, dated 2024-06-14 by `git -C core/deps/discord-rpc log -1 --format='%H %ad %s' --date=iso-strict`. Superproject history (`git log -- core/deps/discord-rpc`) includes an update at `cea80560d` (2024-07-28). `git -C core/deps/discord-rpc ls-remote origin HEAD` failed because DNS for `github.com` was unavailable; current fork and upstream status are unknown.
- The dependency is optional: `USE_DISCORD` defaults to `OFF` (`CMakeLists.txt:128`) and, when enabled, CMake adds and links the submodule (`:1072-1077`). The integration calls the Discord RPC API from `core/ui/discord.cpp` (`:24,54-116`) to initialize, update, and clear presence. No alternate Discord client is vendored.

### Options

**A. Adopt and own the configured fork while it remains in use.** Assign a dependency owner, mirror it under an organization endpoint when approved, monitor security and compatibility notices, and document the patch/rebase policy. **Effort: S. Risk: Low to medium**, because the feature is opt-in, but its IPC and platform behavior still need maintenance.

**B. Replace or remove the integration.** The alternatives are a separately evaluated Discord presence client or removal of the optional feature; neither is in-tree. Changes would center on `core/ui/discord.cpp`, settings/UI call sites, and CMake wiring. **Effort: M. Risk: Medium**, from platform IPC behavior and user-visible loss or changed semantics; the default-off configuration limits core impact.

No upstream-bump option is recommended now because reachability and a newer maintained release could not be verified.

### Recommendation

Propose keeping the feature opt-in and adopting ownership of the current pin until an online upstream/fork comparison is possible. Revisit replacement only if the configured fork cannot be maintained or the integration no longer meets the application's needs.

## breakpad

### Context

- `.gitmodules:13-15` points to `https://github.com/flyinghead/mingw-breakpad`. The pin is `1ab24bcc817ebe629bf77daa53529d02361cb1e9`, dated 2023-05-12 by `git -C core/deps/breakpad log -1 --format='%H %ad %s' --date=iso-strict`. Superproject history (`git log -- core/deps/breakpad`) records an update at `e94babe57` (2023-05-22). `git -C core/deps/breakpad ls-remote origin HEAD` failed because DNS for `github.com` was unavailable; current fork and upstream status are unknown.
- Breakpad is enabled by default except for libretro (`CMakeLists.txt:123,428-436`). Platform startup code uses Breakpad exception handlers on Linux, Android, and Windows (`core/linux-dist/main.cpp:304-319`, `shell/android-studio/flycast/src/main/jni/src/Android.cpp:140-148`, `core/windows/winmain.cpp:377-391`); Apple builds compile selected Breakpad sources (`CMakeLists.txt:437-479`). No crash-reporting replacement is vendored.

### Options

**A. Adopt and own the configured fork.** Assign an owner, mirror/fork under an organization endpoint when approved, track security and platform build fixes, and specify patch and release policy. Maintain a platform matrix for Linux, Windows, Android, and macOS crash capture and symbol generation. **Effort: M. Risk: Medium**, because crash handling is cross-platform and runs in sensitive failure paths.

**B. Replace it.** Crashpad is an external candidate for evaluation; alternatively, platform-native handlers could replace some targets. The change would affect CMake, crash startup/handler code, dump writing, symbol tooling, and platform packaging. **Effort: L. Risk: High**, because dump formats, handler reliability, and symbol processing differ by platform. Verification needs crash-dump tests across supported platforms, which are not available in this spike.

No upstream-bump option is recommended now because reachability and current upstream activity could not be checked.

### Recommendation

Propose adopting ownership of the configured Breakpad fork and retaining the current pin until its remote history can be reviewed. Its default-enabled, multi-platform crash-reporting role makes a replacement too broad to choose without a separate platform test plan.

## luabridge

### Context

- `.gitmodules:10-12` points to `https://github.com/vinniefalco/LuaBridge.git`, and the checked-out repository identifies itself as `2.8-7-g5d21e35`; pin `5d21e35633a1f87ed08af115b07d3386096f792b` is dated 2023-03-04 by `git -C core/deps/luabridge log -1 --format='%H %ad %s' --date=iso-strict`. Superproject history (`git log -- core/deps/luabridge`) records a dependency update at `b2c1d134e` (2024-10-13). `git -C core/deps/luabridge ls-remote origin HEAD` failed because DNS for `github.com` was unavailable.
- The local facts do **not** establish that this URL is a fork-only endpoint: it is the named LuaBridge project repository, and fork/canonical ownership cannot be confirmed offline. CMake uses LuaBridge's headers when Lua support is enabled (`CMakeLists.txt:654-660`); `core/lua/lua.cpp:22-24,37-44` includes the bridge and imports its namespace. Lua itself is a separate dependency resolved by `find_package(Lua 5.2)`.

### Options

**A. Keep and own the current pin.** Retain the exact pin; if a future online check establishes that the endpoint is a personal fork rather than the canonical project repository, mirror it under an organization endpoint and assign an owner, review policy, and compatibility watch. **Effort: S. Risk: Low**, provided Lua script registration behavior is preserved.

**B. Replace the binding layer.** Use Lua's existing C API directly or evaluate another binding library; neither is a new in-tree replacement today. The blast radius is concentrated in `core/lua/` and script registration/callback code. **Effort: L. Risk: High**, because binding conversions, object lifetime, callbacks, and existing user scripts may change.

No upstream-bump option is recommended until the remote and current release history are reachable.

### Recommendation

Propose retaining the current LuaBridge pin and correcting its classification to “fork status unverified” rather than treating it as a confirmed fork-only dependency. Recheck the configured endpoint's canonical ownership and liveness online before deciding whether an organization mirror or upgrade is needed.
