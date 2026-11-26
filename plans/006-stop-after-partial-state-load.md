# 006. Stop emulation after an incomplete state restore

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 6 | P1 | M | Medium | Emulator lifecycle | `ebf8fbb79` · 2026-11-26 |

The slot loader catches a deserialization error and the UI can immediately start the machine again. Some devices have already received state by the time validation fails. The machine must stay stopped and unload without autosaving until a known-good boot or restore establishes a valid session.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/partial-state-load`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/nullDC.cpp' \
  'core/emulator.h' \
  'core/emulator.cpp' \
  'core/ui/gui.cpp' \
  'core/lua/lua.cpp' \
  'tests/src/serialize_test.cpp' \
  'tests/src/ui/LibrarySavestateTest.cpp' \
  'tests/src/RewindTest.cpp'
```

## Current behavior and local conventions

The public slot entry point discards the internal success result. [core/nullDC.cpp:442](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/nullDC.cpp#L442)

```cpp
void dc_loadstate(int index)
{
	loadState(index, "");
}
```

The quick-load path stops and restarts the emulator around this call. [core/ui/gui.cpp:5287](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/gui.cpp#L5287)

```cpp
			emu.stop();
			dc_loadstate(slot);
			emu.start();
```

A late validation can throw after device state has already been applied. [core/serialize.cpp:101](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/serialize.cpp#L101)

```cpp
	int broadcast, cable, region;
	deser >> broadcast;
	deser >> cable;
	deser >> region;
	if (broadcast < 0 || broadcast > 4 || cable < 0 || cable > 3 || region < 0 || region > 3)
		throw Deserializer::Exception("Invalid broadcast, cable or region");
```

Rewind already protects partially restored machines and the next unload. [core/rewind.cpp:440](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/rewind.cpp#L440)

```cpp
		// A partially deserialized machine must not execute another frame.
		if (applying)
		{
			dc_skipAutoSaveOnNextUnload();
			throw FlycastException(i18n::Ts("Failed to restore rewind state"));
```

## Scope

- `core/nullDC.cpp`
- `core/emulator.h`
- `core/emulator.cpp`
- `core/ui/gui.cpp`
- `core/lua/lua.cpp`
- `tests/src/serialize_test.cpp`
- `tests/src/ui/LibrarySavestateTest.cpp`
- `tests/src/RewindTest.cpp`

Keep the state format, library Resume’s boolean public behavior for existing users, achievement restrictions, netplay handshake policy, and rewind semantics. Preserve successful and missing-file behavior for each caller.

## Implementation and verification

### 1. Make the three outcomes observable

Introduce a small result at the common load boundary that distinguishes successful apply, rejection before apply, and failure after application began. Classify file open, header, decompression and constructor checks before mutation. Treat an exception from `emu.loadstate` conservatively as a partial application. Use RAII for the input buffer if propagation changes its lifetime. Add tests beside the existing serialization and library fixtures.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Handle the result at every entry point

Inspect all `dc_loadstate` and `dc_loadstateForPath` callers, including both menu paths, quick-load, startup auto-load, explicit Resume, Lua and the GGPO startup slot. On partial apply, set the one-shot unload suppression before unwinding and enter the established safe stop or startup-failure path. Avoid a second notification or an unconditional resume/toggle. A missing optional network boot state and a rejected library Resume retain their distinct existing outcomes.

Run the **Entry points** command below. The call inventory covers UI, Lua, emulator startup and tests. All edited signatures compile.

### 3. Exercise the lifecycle

Use a normal serialized fixture and a controlled fault after the first mutation through the production apply boundary. Observe restart, unload, the one-shot suppression and success events at the caller. Exercise a second clean session to ensure the flag is consumed once. Use the current rewind test as the behavioral precedent.

Run the **Focused** command below. The new PartialRestoreSuppressesRestartAndAutosave case is registered. The selected tests run and pass.

## Test plan

- Cover success, missing slot, rejected header before apply and controlled post-mutation failure. On late failure expect zero subsequent guest frames, zero automatic writes and zero successful LoadState events.
- Test both numbered slot and explicit path. Confirm manual loading can resume a still-valid machine after early rejection, while explicit library Resume keeps its existing return-to-library behavior.
- Cover automatic startup, memory slot and .net routing with fixtures or dispatch-policy tests. Confirm a fresh session again allows the configured autosave.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='SerializeTest.DreamcastHeaderMatchesDevV63:LibrarySavestateTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='SerializeTest.*:LibrarySavestateTest.*:RewindTest.*' | rg -q '^[[:space:]]+PartialRestoreSuppressesRestartAndAutosave([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='SerializeTest.*:LibrarySavestateTest.*:RewindTest.*'
```

The new PartialRestoreSuppressesRestartAndAutosave case is registered. The selected tests run and pass.

**Entry points**

```sh
rg -n 'dc_loadstate(ForPath)?\(' core shell tests && cmake --build --preset linux-unit-tests-debug --parallel 4
```

The call inventory covers UI, Lua, emulator startup and tests. All edited signatures compile.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The focused suite distinguishes at least the three load outcomes and observes the no-restart/no-autosave result through the caller. Existing valid-state byte-layout tests pass.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `57aad7a8`. The numbered-slot UI resumed a running synthetic guest after an invalid header and emitted Pause then Resume. A partial restore blocked start, returned to the library, emitted Terminate and preserved the old autosave bytes. A fresh session wrote and reloaded its own autosave. The regression passed on macOS and Ubuntu arm64. Teardown now joins the emulation work before clearing the failure flags.

## Stop conditions

If a caller invokes the load from the emulation thread, do not synchronously join that thread from its own failure path. Trace the thread’s existing fatal-error handoff and verify it before wiring that caller. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Apply the same result policy to future frontend entry points. The save-slot writer may change independently, so keep failure injection on the read/apply side.
