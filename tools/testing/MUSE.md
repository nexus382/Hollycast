<!-- Portions Copyright 2026 The Hollycast Authors -->
# Muse CLI play-testing

Muse and Codex use the same native harness for supported standalone Windows/Linux
builds. The interface is ordinary Node CLI
commands and JSON/JSONL, with no Codex SDK, desktop-control plugin, or model API
dependency. The conversation bridge carries the assignment and the report;
the live data goes directly between Muse's command tool and the emulator.

For a single-run current-game capture, use the `benchmark` command described in
README.md. It defaults to sixty measured seconds after startup and warm-up,
retains a human-readable report, and publishes `benchmark-status.json`. It does
not establish game-specific gameplay semantics. Use
`status --run DIRECTORY` for monitoring without extra emulator queries. With
`--control external`, Muse can drive through the normal controller commands
while the benchmark collects results. A native build advertising
`benchmark_frame_counts` and `exclusive_ui_input` is required. Include the report
paths and tested conditions when returning results through the bridge. Planet
Harriers workflows require a Hikaru-enabled branch, which current dev does not
include. This native harness does not cover device, libretro, or frontend builds.

Prepare an isolated session, then create a concrete job:

```powershell
node tools/testing/hollytest.mjs doctor --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs job --run D:/tmp/hollytest-session-001 --out D:/tmp/hollytest-job.json --workspace D:/path/to/Hollycast
```

The job binds the disposable session to its PID, game ID, executable hash, and
declared source revision. It includes literal command argument arrays for both
Windows and WSL, a time budget, permitted observation/control scope, and the
result contract, plus the active profile identity and mapped field names.
It contains no native session nonce and does not send a message,
create a Muse chat, or transfer bridge ownership by itself.

Session files remain as evidence after the emulator exits. A ready file or job
does not establish a running process: `doctor` must complete its live native
handshake. If it fails, report the unavailable session; prepare a new disposable
run and job before another assignment. Do not replay commands with unknown outcomes.

For a Windows emulator session, Muse in WSL should invoke the **Windows Node
executable** through its /mnt/c path and keep the script/session arguments as
Windows paths. The generated job supplies this form. Linux Node must not invent
a second session by interpreting a Windows manifest as Linux paths.

When the user requests offloading, follow the installed conversation-bridge
skill. Use the current real session IDs and the same bridge workspace on both
sides. Include the job path, expected executable identity, objective, and the
authorized play-test steps in fresh bridge context. Do not transfer full logs or
credentials. Persistent bridge delivery requires **bridge pull** in the intended
existing Muse CLI; it does not automatically wake that CLI or continuously sync
transcripts. Do not relink an existing pairing or invent a recipient session.

After accepting the transfer, Muse should:

1. Read the job and run its doctor/verify commands before controlling anything.
2. Read live JSON and native ImGui items, then issue bounded controller/UI actions.
   The `hold`/`controller --state JSON` commands cover both sticks, triggers,
   simultaneous buttons, and ports. `ui --input JSON` covers native click,
   key, text, scroll, and drag actions. Job command arrays include examples.
3. Use native screenshots only when structured data cannot answer the question.
4. Keep all changes inside the disposable session; do not edit emulator source,
   replace the tested executable, change original saves, or control another run.
5. Retain reproducible steps, timestamps, observed outcomes and artifact paths.
   Report unmapped game fields as unknown and incomplete testing as inconclusive.
6. Release all inputs, verify original hashes, and return the concise report and
   evidence pointers through the bridge using the receiving skill's send/pull flow.

Only one agent controls a given session. Codex can use a separate immutable build
and session for independent testing, but must respect the bridge's conversation
ownership rules. A bridge context transfer does not transfer live processes;
the explicit session directory and identity let the receiver attach to this run.
