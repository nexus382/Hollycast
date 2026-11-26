# 004. Back up legacy controller mappings byte for byte

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 4 | P1 | S | Low | Controller migration | `ebf8fbb79` · 2026-11-26 |

Loading an older controller profile can create or truncate its `.save` file. The first fix corrected the source/destination handle mix-up, but review found two further preservation failures: an empty artifact beside a legacy source prevented backup creation, and a failed copy or finalization could leave bytes that later loads trusted only because the path existed. A previous good legacy `.save` may contain different bindings and must remain byte-for-byte intact.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/legacy-mapping-backup`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/input/mapping.cpp' \
  'tests/src/input/InputMappingConfigFileTest.cpp'
```

## Current behavior and local conventions

Version 1 and 2 files use the legacy parser. It marks the mapping dirty. [core/input/mapping.cpp:463](../core/input/mapping.cpp#L463)

```cpp
	version = mf.getInt("emulator", "version", 1);
	if (version < 3)
	{
		loadv1(mf);
		return;
```

The initial fix reads the source and writes the backup through separate handles, but creates the final `.save` path directly and then checks the reopened copy. [core/input/mapping.cpp](../core/input/mapping.cpp)

```cpp
			std::unique_ptr<hostfs::File> source(hostfs::storage().openFile(path, "rb"));
			std::unique_ptr<hostfs::File> out(hostfs::storage().openFile(backupPath, "wb"));
```

The existing fixture creates real mapping files under the writable config root. [tests/src/input/InputMappingConfigFileTest.cpp:22](../tests/src/input/InputMappingConfigFileTest.cpp#L22)

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
- `plans/004-back-up-legacy-mappings.md` (this worktree copy of the untracked plan)

Keep `.save` beside the selected source profile, preserve custom-folder lookup and retain the mapping format and existing best-effort return of a usable parsed mapping.

## Implementation and verification

### 1. Exercise recovery through `LoadMapping`

Extend `InputMappingConfigFileTest` with raw binary read/write helpers for temporary profiles. Clear the static mapping cache between loads, restore custom mapping roots, and remove source, backup, and transaction artifacts in teardown. Exercise `InputMapping::LoadMapping` by filename. Cover an empty pre-existing `.save`, a distinct valid historical backup, a blocked destination followed by a successful retry, plus v1/v2/default/custom lookups.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Publish only verified bytes

Read and validate the full source before opening a destination. For ordinary native paths, write to a sibling temporary file, reopen and compare all bytes, then publish with a rename. Replace only an invalid or empty artifact; keep a non-empty legacy backup byte-for-byte even when its contents differ from the source. For storage providers without rename/remove operations, record pending/complete state beside the backup so a subsequent load retries bytes left by an interrupted write. Keep `.save` beside the resolved profile path, including configured roots, and report failures through the existing input warning path.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Verify migration and retry behavior

Load legacy profiles through the normal entry point, check the returned controls, compare backup bytes, save the converted profile, and reload with a cleared cache. Verify the backup still contains the original bytes. Confirm an empty `.save` is repaired, a different valid legacy backup is preserved, a blocked destination leaves the source intact and succeeds after retry, and current-format loads do not create migration backups.

Run the **Focused** command below. The new LegacyBackupPreservesOriginalBytes case is registered. The selected tests run and pass.

## Test plan

- Use version 1 and version 2, a custom root and default lookup, CRLF plus comments, no final newline and a file longer than the 4096-byte copy buffer.
- Repair a pre-existing zero-byte `.save`; preserve a distinct, valid v1 backup byte-for-byte.
- Exercise destination failure and retry with an isolated obstruction fixture. Preserve the source and make the failure observable. Avoid permissions-only tests that succeed under elevated users.
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
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='InputMappingConfigFileTest.*' | rg -q '^[[:space:]]+LegacyBackupPreservesOriginalBytes([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='InputMappingConfigFileTest.*'
```

The LegacyBackupPreservesOriginalBytes case and the complete `InputMappingConfigFileTest.*` suite run and pass.

### Observed in this revision

- Host: macOS, arm64. A standalone Ninja Debug build under `/private/tmp` succeeded with `ENABLE_CTEST=ON`, Vulkan and Breakpad disabled, and Discord disabled. The normal Darwin preset could not configure in this sandbox because Xcode tried to write to the user DerivedData directory; the preset also requires an unavailable MoltenVK dylib for Vulkan.
- Focused result: all 4 `InputMappingConfigFileTest.*` tests passed.
- `git diff --check` passed. The parent run handles aggregate Linux checking.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named test compares bytes, covers an empty artifact, distinct historical backup, retry path, a multiple-chunk source, and saving the upgraded profile while retaining a usable recovery copy.
- [x] The macOS arm64 focused build and suite passed; aggregate Linux checking is handled by the parent run.
- [x] The **Scope** gate matches the revised implementation, test, and this plan copy. The plan index was not changed.

## Stop conditions

If a storage backend requires a new replacement API to preserve an existing backup, use an available checked storage operation or provider recovery state and document the backend limitation. Do not discard a good backup as an error-recovery shortcut. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Keep binary-copy tests separate from the INI serializer’s text comparisons. Future mapping versions should take the backup path only when migration marks a profile dirty.
