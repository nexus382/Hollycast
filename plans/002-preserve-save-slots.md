# 002. Preserve the previous save slot until its replacement is complete

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 2 | P1 | M | Medium | Persistence | `ebf8fbb79` · 2026-11-26 |

A manual save or unload autosave can erase a working slot on a full disk. A late error can also display “State saved.” A successful overwrite must leave one loadable state at the same slot path. A failed overwrite must leave the previous state recoverable.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/atomic-save-slots`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/nullDC.cpp' \
  'core/archive/rzip.cpp' \
  'core/archive/rzip.h' \
  'core/oslib/storage.h' \
  'core/oslib/storage.cpp' \
  'shell/libretro/storage.h' \
  'shell/libretro/storage.cpp' \
  'shell/android-studio/flycast/src/main/jni/src/android_storage.h' \
  'shell/android-studio/flycast/src/main/java/com/hollycast/emulator/AndroidStorage.java' \
  'shell/android-studio/flycast/src/main/java/com/hollycast/emulator/SaveSlotJournal.java' \
  'shell/android-studio/flycast/src/test/java/com/hollycast/emulator/SaveSlotJournalTest.java' \
  'shell/android-studio/flycast/src/androidTest/java/com/hollycast/emulator/AndroidStorageTest.java' \
  'tests/src/oslib/SavestateWriteTest.cpp' \
  'tests/src/LibretroStorageTest.cpp' \
  'CMakeLists.txt' \
  'tests/CMakeLists.txt'
```

## Current behavior and local conventions

The disk branch opens the final slot before writing the header, optional PNG and RZIP payload. [core/nullDC.cpp:270](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/nullDC.cpp#L270)

```cpp
		// regular file savestate
		filename = hostfs::getSavestatePath(index, true);
		f = hostfs::storage().openFile(filename.c_str(), "wb");
```

The RZIP writer owns the stream after Open succeeds. Close patches the uncompressed size and releases the file. [core/archive/rzip.cpp:84](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/archive/rzip.cpp#L84)

```cpp
		if (write)
		{
			file->seek(startOffset + sizeof(RZipHeader) + sizeof(maxChunkSize), SEEK_SET);
			file->write(&size, sizeof(size), 1);
		}
		delete file;
```

The native rename wrapper already supplies replacement semantics on Windows. [core/oslib/directory.cpp:302](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/oslib/directory.cpp#L302)

```cpp
    }
	// _wrename() fails when newpath exists. Replace it, as POSIX rename() does.
	if (!MoveFileExW(woldpath.get(), wnewpath.get(), MOVEFILE_REPLACE_EXISTING)) {
		const DWORD error = GetLastError();
```

The existing metadata writer checks write, flush and close separately. Apply those checks to save states. [core/ui/boxart/boxart.cpp:221](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/boxart.cpp#L221)

```cpp
	if (file == nullptr)
		return false;
	bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
	if (std::fflush(file) != 0)
		written = false;
	if (std::fclose(file) != 0)
		written = false;
```

## Scope

- `core/nullDC.cpp`
- `core/archive/rzip.cpp`
- `core/archive/rzip.h`
- `core/oslib/storage.h`
- `core/oslib/storage.cpp`
- `shell/libretro/storage.h`
- `shell/libretro/storage.cpp`
- `shell/android-studio/flycast/src/main/jni/src/android_storage.h`
- `shell/android-studio/flycast/src/main/java/com/hollycast/emulator/AndroidStorage.java`
- `shell/android-studio/flycast/src/main/java/com/hollycast/emulator/SaveSlotJournal.java`
- `shell/android-studio/flycast/src/test/java/com/hollycast/emulator/SaveSlotJournalTest.java`
- `shell/android-studio/flycast/src/androidTest/java/com/hollycast/emulator/AndroidStorageTest.java`
- `tests/src/oslib/SavestateWriteTest.cpp` (new)
- `tests/src/LibretroStorageTest.cpp` (new)
- `CMakeLists.txt` and `tests/CMakeLists.txt`

Keep slot names, PNG layout, on-disk format, legacy reads, and current user-selected storage roots compatible. RAM slot −2 uses a memory stream where supported. Frontend-owned libretro serialization remains a separate API.

## Implementation and verification

### 1. Characterize the write contract

Register `SavestateWriteTest` alongside the existing storage tests. Use a temporary profile, a known-good slot, a small second payload and a `hostfs::File` test double with independently controllable write, seek and finalization results. Put the named overwrite regression at the same transaction boundary the production save will call.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Make completion observable

Give the RZIP writer an explicit checked finish. Verify the final seek, size write, and underlying flush/close. Preserve the destructor as a safe cleanup path and release each stream exactly once, including when Open fails. Update native, memory and VFS file wrappers as needed. A successful checked finish must still produce bytes the current RZIP reader consumes.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Publish disk slots transactionally

For native paths, create an exclusive sibling in the destination directory, write every part, finish, then use the platform replacement primitive. Distinct attempts must own distinct scratch files. Resolve custom storage through its provider. On a supported provider without replace-over-existing, stage a fully verified document and preserve enough information to recover the old or new slot after interruption. Check what is possible before opening the live document. Test overwrites on a provider that currently supports them, as well as the explicit failure when a provider denies the operation. Announce success only after publication. Clean up only files belonging to this attempt.

Run the **Focused** command below. The new ExistingSlotSurvivesFailedCommit case is registered. The selected tests run and pass.

### 4. Exercise provider-specific completion

Run the new instrumentation case with a writable disposable Android document tree, plus the desktop and VFS builds below. On Windows, exercise replacing an existing slot with the native wrapper. If a provider implements multiple publication steps, interrupt each transition in the fixture, reopen the slot and compare against both known complete images.

Run the **Android document test**, **Windows build** and **VFS build** commands on their named hosts. The document test must execute its overwrite and restart checks.

## Test plan

- Successful save with and without a preview reopens through the normal loader. Check header, PNG boundaries and decompressed bytes, including a replacement with a shorter payload.
- Inject short writes at the header, preview, payload and final length, then a failed seek, close and replacement. Assert original bytes and creation date survive, a new failed slot is absent, and a subsequent save succeeds.
- Exercise two overlapping attempts with independent scratch names. Cover autosave’s disk path and the memory-slot success path. On a disposable Android document tree exercise new slot, overwrite, denied write and reopening after interruption.

- Exercise a complete disk save through the emulator. Limit the next write, reopen the previous slot, check its bytes and modification time, and make a successful retry.
- On native filesystems, compare the original and replacement access mode, owner, group and applicable access controls. Include a private slot in a directory with inherited access controls.
- Seed an interrupted replacement in an Android document tree or a Windows libretro frontend. Look up the slot through the public storage interface after restart. Include a fresh slot in a frontend-only directory, a failed overwrite, and a controller backup using the same publication operation.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='SerializeTest.DreamcastHeaderMatchesDevV63'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='SavestateWriteTest.*:SerializeTest.*' | rg -q '^[[:space:]]+ExistingSlotSurvivesFailedCommit([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='SavestateWriteTest.*:SerializeTest.*'
```

The new ExistingSlotSurvivesFailedCommit case is registered. The selected tests run and pass.

**Android document test**

```sh
(cd shell/android-studio && ./gradlew :flycast:testDebugUnitTest :flycast:connectedDebugAndroidTest '-Pandroid.testInstrumentationRunnerArguments.class=com.hollycast.emulator.AndroidStorageTest')
```

Run the four storage instrumentation cases on a disposable device or emulator with a writable document tree. They exercise normal storage operations, canonical child URIs, recovery after interrupted replacement and opening a single shared file. Record the provider and Android version.

**Windows build**

```sh
cmake --preset windows-unit-tests-debug && cmake --build --preset windows-unit-tests-debug
```

The native Windows file wrapper builds. Run `SavestateWriteTest.*` using the generated Debug Hollycast test executable.

**VFS build**

```sh
cmake -S . -B build-save-vfs -DLIBRETRO=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-save-vfs --target Hollycast_libretro --parallel 4
ctest --test-dir build-unit-tests-debug --output-on-failure -R '^libretro_storage$'
```

The libretro build compiles the VFS stream and file ownership changes. The native unit-test build runs the callback regressions through CTest. They cover overwrite recovery through public lookups, an overlapping reader, and routing back to native storage when the frontend declines VFS.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named regression asserts the old slot remains byte-for-byte readable after a failed commit. Every late failure has an error outcome. Successful saves retain the existing reader and thumbnail format.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `e3f9a7d6`. On macOS and Ubuntu, a real emulator save survived a forced file-size limit with identical bytes and modification time, then reloaded and accepted the next save. Native checks covered access controls and a Linux setgid parent. Six libretro regressions passed twice on Windows x64 under Wine. Stock frontend callbacks recovered interrupted overwrites and could create a slot in a frontend-owned directory. The Android SDK built both APKs, 27 JVM tests passed, and all four production storage cases passed on Android 15 arm64 with the external storage document provider. Five Windows API-model checks under Wine covered access rules, neighboring files, errors 1176 and 1177, and a real writer process interrupted during journal creation. The Windows Store SDK built and packaged the application for Win32, x64 and ARM64. [UWP SDK build](https://github.com/nexus382/Hollycast/actions/runs/45910521617)

## Stop conditions

If a document provider cannot restore the prior slot after an interrupted overwrite, record its capabilities and keep its live file intact. A platform fallback that silently redirects the player’s saves requires a storage-policy decision. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Review the ownership handoff at RZIP Open and every save exit together. The checked file API also affects libretro VFS and Android descriptor-backed streams.
