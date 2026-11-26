# 004. Back up legacy controller mappings byte for byte

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 4 | P1 | S | Low | Controller migration | `ebf8fbb79` · 2026-11-26 |

Loading an older controller profile creates or truncates its .save file and then writes to the input stream. In an isolated check, a 45-byte input produced an empty backup. Players need the original bindings when a format upgrade goes wrong.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/legacy-mapping-backup`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

The legacy mapping tests can start independently. Publishing the verified copy uses the checked storage operation from [plan 002](002-preserve-save-slots.md). Compare the scoped history against the baseline.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/input/mapping.cpp' \
  'tests/src/input/InputMappingConfigFileTest.cpp'
```

## Current behavior and local conventions

Version 1 and 2 files use the legacy parser. It marks the mapping dirty. [core/input/mapping.cpp:463](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/input/mapping.cpp#L463)

```cpp
	version = mf.getInt("emulator", "version", 1);
	if (version < 3)
	{
		loadv1(mf);
		return;
```

The backup loop reads and writes through the same handle. [core/input/mapping.cpp:887](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/input/mapping.cpp#L887)

```cpp
				{
					size_t n = fp->read(buf, 1, sizeof(buf));
					if (n <= 0)
						break;
					fp->write(buf, 1, n);
				}
```

The existing fixture creates real mapping files under the writable config root. [tests/src/input/InputMappingConfigFileTest.cpp:22](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/src/input/InputMappingConfigFileTest.cpp#L22)

```cpp
    void writeConfigFile(const std::string& contents)
    {
        std::filesystem::create_directories(getConfigFilepath().parent_path());
        std::ofstream configFile(getConfigFilepath());
        configFile << contents;
        configFile.close();
        ASSERT_TRUE(configFile) << getConfigFilepath();
    }
```

## Scope

- `core/input/mapping.cpp`
- `tests/src/input/InputMappingConfigFileTest.cpp`

Keep `.save` beside the selected source profile, preserve custom-folder lookup and retain the mapping format and existing best-effort return of a usable parsed mapping.

## Implementation and verification

### 1. Add a real legacy profile fixture

Extend `InputMappingConfigFileTest` with raw binary read/write helpers for temporary profiles. Clear the static mapping cache between loads, restore custom mapping roots, and remove both the source and `.save` in teardown. Exercise `InputMapping::LoadMapping` by filename.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Verify the staged copy before publication

Use the selected provider’s parent and display name to find the sibling backup. Preserve any nonempty backup and leave an unreadable backup untouched. Repair a confirmed zero-byte file left by the old writer. Copy the source in bounded chunks to a temporary file using the checked storage operation from [plan 002](002-preserve-save-slots.md). Check read, write, flush and close results, then reopen and compare the staged bytes with the source. Recheck the destination before publishing. Report failures through the input warning path and return the usable mapping.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Verify both formats and the normal save flow

Load a legacy profile, check the returned controls, compare its backup against the original byte string, then save the converted profile. Reload using a cleared cache. The recovery copy must still contain the original bytes. Run the equivalent current-format load and assert no migration backup is created.

Run the **Focused** command below. The new LegacyBackupPreservesOriginalBytes case is registered. The selected tests run and pass.

## Test plan

- Use version 1 and version 2, a custom root and default lookup, CRLF plus comments, no final newline and a file longer than the 4096-byte copy buffer.
- Simulate a backup destination failure with an isolated fixture, including a pre-existing good backup. Preserve the source and make the failure observable. Avoid permissions-only tests that succeed under elevated users.
- Exercise an opaque provider path, a short write, a failed flush, a failed close and a failed read of an existing backup. Retry each staging failure and check that a historical backup appearing during the copy survives.
- Current v4 and a second load from the in-memory cache retain their existing behavior.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='InputMappingConfigFileTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='InputMappingConfigFileTest.*' | rg -q '^[[:space:]]+LegacyBackupPreservesOriginalBytes([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='InputMappingConfigFileTest.*:InputMappingBackupProviderTest.*'
```

The new LegacyBackupPreservesOriginalBytes case is registered. The selected tests run and pass.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named test compares bytes, covers a multiple-chunk source, and verifies that saving the upgraded profile leaves a usable recovery copy.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `8ac4a52f`. Seven mapping and provider regressions, plus two independent LoadMapping fixtures, passed on macOS and Ubuntu arm64. They compared legacy bytes across CRLF, a missing final newline and multiple copy buffers, repaired an empty backup, and preserved an unreadable or newly appearing historical backup. Stock Windows libretro callbacks under Wine recovered the old .cfg.save through the first exists, metadata and open calls. The Android 15 document-provider instrumentation also recovered a controller backup.

## Stop conditions

If a storage backend requires a new replacement API to preserve an existing backup, use an available checked storage operation or keep that copy and document the backend limitation. Do not discard a good backup as an error-recovery shortcut. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Keep binary-copy tests separate from the INI serializer’s text comparisons. Future mapping versions should take the backup path only when migration marks a profile dirty.
