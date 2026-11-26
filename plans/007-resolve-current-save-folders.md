# 007. Resolve quick-load against the current save folders

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 7 | P1 | S | Low | Storage paths | `ebf8fbb79` · 2026-11-26 |

After the player replaces or reorders the configured directories, revisiting the same slot can return a path in a removed directory. An isolated check returned the old Game.state even though the newly selected folder contained its own copy.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/refresh-savestate-paths`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/oslib/oslib.cpp' \
  'tests/src/ui/LibrarySavestateTest.cpp'
```

## Current behavior and local conventions

The live resolver keeps process-wide mutable state keyed by filename. [core/oslib/oslib.cpp:195](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/oslib/oslib.cpp#L195)

```cpp
	const std::string state_file = makeSavestateFileName(settings.content.fileName, index);

	static std::string lastFile;
	static std::string lastPath;
```

A substring comparison can accept a cached path from a different root. [core/oslib/oslib.cpp:219](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/oslib/oslib.cpp#L219)

```cpp
			else if (lastPath.find(userPath) != std::string::npos)
			{
				return lastPath; // Return cached path if it's in this user path
			}
```

The existing fixture explicitly clears this cache by performing a writable lookup. [tests/src/ui/LibrarySavestateTest.cpp:123](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/src/ui/LibrarySavestateTest.cpp#L123)

```cpp
	std::filesystem::remove(stateFile(gameDirectory));
	(void)hostfs::getSavestatePath(0, true); // Clear the live resolver's path cache for this fixture.
	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 0, hostfs::getSavestateFolders()));
```

The settings UI promises ordered lookup and writes to the first folder. [core/ui/settings_new.cpp:5493](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/settings_new.cpp#L5493)

```cpp
				"Folders used for save states.\n\n"
				"The first folder is used when creating new save states.\n"
				"All listed folders are searched when loading, which is useful if you keep states organized across different locations.\n"
```

## Scope

- `core/oslib/oslib.cpp`
- `tests/src/ui/LibrarySavestateTest.cpp`

Keep slot filename conventions, .net and .tmp variants, root order and the independent asynchronous library metadata cache. Restrict this change to selecting a path for the active game.

## Implementation and verification

### 1. Capture folder changes without a cache reset

Extend the real-directory fixture with two folders containing the same slot and distinct bytes. Perform two successive read-only resolutions around a folder change. Define another case for a removed file with a lower-priority copy available. Clear any artificial writable lookup between the two observations.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Replace the stale filename cache

Prefer removing the two static strings from `getSavestatePath`. For reads, walk custom folders in order via hostfs, then use the existing live default resolver. Keep writable resolution cheap and preserve the fallback when the destination does not exist. Share code with the library resolver only where empty content, default-user paths and provider URI semantics match.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Check the filesystem and library paths

Cover root reordering, an empty custom list, similarly named sibling roots, deleted files, the same filename used by different games, and a nonzero slot. Compare the path returned by the live and library resolvers for equivalent active content and folders.

Run the **Focused** command below. The new ChangingRootsRechecksSameSlot case is registered. The selected tests run and pass.

## Test plan

- Exercise the same slot twice without a save between calls. Assert the new root wins, including when the old file still exists.
- Remove the first matching slot and assert the next available file wins. For a missing state check the writable fallback. For a write check the first configured usable root without requiring the file to exist.
- Keep the library scan’s nonblocking, generation and bounded-queue tests passing. The fixture must restore the original data and config directories.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='LibrarySavestateTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='LibrarySavestateTest.*:LibrarySavestateCacheTest.*' | rg -q '^[[:space:]]+ChangingRootsRechecksSameSlot([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='LibrarySavestateTest.*:LibrarySavestateCacheTest.*'
```

The new ChangingRootsRechecksSameSlot case is registered. The selected tests run and pass.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named test and deletion/reordering cases pass without using a writable lookup as an invalidation trick. Library metadata inspection still leaves the active game settings intact.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `5c02b77dd`. All 20 library savestate tests and six cache tests passed on macOS and Ubuntu arm64. Reordering roots and deleting the first candidate both changed the next read of the same slot. The writable path and missing-slot fallback also passed.

## Stop conditions

If a shared resolver changes the empty-game or BIOS fallback, keep the existing separate call paths and remove the stale live cache locally. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

A future lookup cache would need the active content, root order and file existence changes in its invalidation model. A filename alone cannot establish those.
