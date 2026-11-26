# 005. Verify migrated VMU cards before retiring their source

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 5 | P1 | M | Medium | VMU persistence | `ebf8fbb79` · 2026-11-26 |

An opened legacy card is treated as a successful import even after a short read. Setup can then write a partial 128 KiB image and remove the only original. A retry needs either the verified new card or the original with its bytes intact.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/vmu-migration-integrity`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/hw/maple/maple_devs.cpp' \
  'core/hw/maple/maple_devs.h' \
  'core/oslib/vmu_path.cpp' \
  'tests/phone_vmu/generate_maple_sync_test.py' \
  'tests/src/oslib/VmuPathTest.cpp' \
  'tests/src/hw/VmuMigrationTest.cpp' \
  'tests/CMakeLists.txt'
```

## Current behavior and local conventions

Card setup logs an incomplete source read and continues. [core/hw/maple/maple_devs.cpp:463](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/hw/maple/maple_devs.cpp#L463)

```cpp
	else
	{
		if (rfile->read(flash_data, sizeof(flash_data), 1) != 1)
			WARN_LOG(MAPLE, "Failed to read the VMU file \"%s\" from disk", rpath.c_str());
		delete rfile;
```

Destination creation currently gates removal on fullSave, which only checks fwrite. [core/hw/maple/maple_devs.cpp:480](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/hw/maple/maple_devs.cpp#L480)

```cpp
			else if (hasExistingImage)
			{
				// VMU file is being renamed so save it fully now
				// and delete the old file
				if (fullSave())
					nowide::remove(rpath.c_str());
```

Older per-game A1 cards legitimately resolve to the ROM-based path. [core/oslib/vmu_path.cpp:116](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/oslib/vmu_path.cpp#L116)

```cpp
	if (port == "A1" && config::PerGameVmu && !settings.content.path.empty())
	{
		// Older standalone releases named per-game cards after the ROM.
		const auto legacy = get_game_save_prefix() + perGameVmuSuffix;
		if (file_exists(legacy))
			return legacy;
```

Use the existing temporary data and custom-card directories for fixtures. [tests/src/oslib/VmuPathTest.cpp:26](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/src/oslib/VmuPathTest.cpp#L26)

```cpp
		fs::create_directories(data);
		set_user_data_dir(data.string());
		config::VMUPath = custom.string();
		config::PerGameVmu = false;
```

## Scope

- `core/hw/maple/maple_devs.cpp`
- `core/hw/maple/maple_devs.h`
- `core/oslib/vmu_path.cpp`
- `tests/phone_vmu/generate_maple_sync_test.py`
- `tests/src/oslib/VmuPathTest.cpp`
- `tests/src/hw/VmuMigrationTest.cpp` (new)
- `tests/CMakeLists.txt`

Keep the card naming and lookup order, 128 KiB flash layout, normal block I/O, Phone VMU publication, and GGPO guest memory-only policy. All fixtures use private temporary directories.

## Implementation and verification

### 1. Set up realistic card fixtures

Create a patterned card with identifiable first and last blocks under the legacy name and use real `getVmuPath` resolution. Instantiate the production VMU setup path. Track source, destination, in-memory data and the reopened backing handle. Use deterministic short-read/write adapters where filesystem permissions would be unreliable.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Distinguish absence from a failed import

Track complete reads separately from an opened stream. Check for an existing unreadable source through hostfs when possible. Keep fresh-card initialization for genuinely absent cards and the existing policy for valid empty cards. An incomplete import must not enter the blank-card initializer, create a preferred-path placeholder, or open the failing source for update.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Commit and validate the migration

Write the complete image into an exclusively owned sibling of the native destination, check flush and close, reopen and verify the 128 KiB payload, and publish using an operation that preserves a destination that appeared concurrently. Open the committed backing file for normal block I/O before retiring the distinct, still-identical source. Where the provider cannot establish identity or delete safely, retain the old card. Exercise failure, retry and the next block write.

Run the **Focused** command below. The new ShortSourceSurvivesMigration case is registered. The selected tests run and pass.

## Test plan

- Verify valid shared legacy and per-game A1 moves preserve every byte. A current-path card and a fresh card still open normally.
- Test partial and unreadable legacy input, failed destination write/finalization, failed reopen, and a destination already present or appearing during migration. Assert the source survives failures and the next startup can retry.
- Verify GGPO guest setup can read initial card data and makes no persistent migration changes. Preserve deliberate memory-only behavior independently of a failed migration.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='VmuPathTest.*:MapleGgpoTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='VmuMigrationTest.*:VmuPathTest.*:MapleGgpoTest.*' | rg -q '^[[:space:]]+ShortSourceSurvivesMigration([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='VmuMigrationTest.*:VmuPathTest.*:MapleGgpoTest.*'
```

The new ShortSourceSurvivesMigration case is registered. The selected tests run and pass.

**Standalone phone VMU**

```sh
cmake -S tests/phone_vmu -B build/phone-vmu-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/phone-vmu-tests --config Release --parallel 4
ctest --test-dir build/phone-vmu-tests -C Release --output-on-failure
```

With Node.js 24 available, all eight cases run, including the server test that exercises Maple card synchronization.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The old image is present and byte-identical for each failed migration. A successful move reopens the expected image and a subsequent block write reaches the correct backing file.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `e2355410`. On a mounted exFAT volume, hard-link publication failed as expected. A Maple block then survived a restart through the verified writable legacy card. Conflicting destinations and failed reopens returned a device write error and preserved memory. The VMU, path and GGPO regressions passed on macOS and Ubuntu arm64, including an isolated restart test.

PR CI found that the standalone phone-VMU fixture also copied the new migration helpers. Reviewed `ff7ac345d`, which extracts the two production methods the fixture calls. The generated C++ matched the working `dev` fixture apart from whitespace, including after an unrelated definition was inserted beside each method. The standalone Release build and all eight CTests passed on macOS and Ubuntu arm64.

## Stop conditions

If two path strings may identify the same backing object, retain the source until equivalence is resolved. If the fixture uses the user’s normal VMU search roots, isolate it before running. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

A buffered full-card write used during migration needs an explicit completion check. Avoid adding an expensive flush to every ordinary VMU block write.
