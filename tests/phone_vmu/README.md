# Phone VMU checks

These tests compile the production phone service and HTTP server. They need CMake,
a C++17 compiler, the Asio submodule, and a local network interface. They listen on
an ephemeral TCP port and make requests over loopback. No games or save files are
read. Node.js additionally enables browser protocol/rendering checks.

```sh
cmake -S tests/phone_vmu -B build/phone-vmu-tests
cmake --build build/phone-vmu-tests --config Release
ctest --test-dir build/phone-vmu-tests -C Release --output-on-failure
```

Coverage includes pre-pairing publication, all slot boundaries, LCD bit order,
blank/inactive frames, independent slots, unpaired/malformed URLs, read-only
requests, cache/privacy headers, activity reporting, stopping with partial
requests, closing the listener, and invalidating pairing links on restart.
The browser check exercises the actual embedded script, including stale responses,
inactive displays, retries, rotation, hidden tabs, A+B multi-touch, cancelled/lost
pointers, keyboard/touch overlap, lost focus, and switching to/from mini-games.
The mini-game check runs a generated LC86K program that copies the input register
to flash and LCD. It verifies CPU execution, input, sleeping, waking, source-copy
isolation, card/game validation, and invalid BIOS sizes without external ROM files.
The wake-lock check covers native acquisition and refusal, opt-out, backgrounding,
history restoration, pending-request races, VMU Sleep, and the user-activated
silent-video fallback used on local HTTP. The server also verifies that the
fallback's media and MIT license are served with a compatible media policy.

For browser acceptance without a game, run `phone_vmu_server --preview` and open
the printed address. A synthetic LCD is served in A1 and a test pattern in D2.
Press Enter to stop. This verifies the transport and browser; playing a real game
on a physical phone is a separate acceptance check.

An optional local `.vms` can be checked and converted to a preview card:

```sh
node tests/phone_vmu/mini_game.mjs /path/to/game.vms build/preview-card.bin
build/phone-vmu-tests/phone_vmu_server --preview build/preview-card.bin
```

This path is useful with Marcus Comstedt's public-domain VMU Tetris example.
The game is not bundled in the source tree.
