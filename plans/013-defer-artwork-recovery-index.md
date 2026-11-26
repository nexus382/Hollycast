# 013. Skip the artwork image index when database recovery is unnecessary

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 13 | P2 | S | Low | Library performance | `ebf8fbb79` · 2026-11-26 |

On the first metadata lookup with the normal writable artwork root, recovery enumerates the directory tree for databases and then enumerates it again to build an image index. In a normal library with at most one database, that index is immediately discarded. The duplicate work is visible in source. The cold-start timing benefit needs measurement.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `perf/defer-artwork-recovery-index`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/ui/boxart/boxart.cpp' \
  'core/ui/boxart/boxart.h' \
  'tests/src/ui/BoxartTest.cpp'
```

## Current behavior and local conventions

The first recursive traversal looks for database names. [core/ui/boxart/boxart.cpp:1903](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/boxart.cpp#L1903)

```cpp
		hostfs::DirectoryTree tree(databaseDir);
		for (auto it = tree.begin(); it != tree.end(); ++it)
		{
			const hostfs::FileInfo& entry = *it;
```

The second traversal walks the artwork tree and indexes images. [core/ui/boxart/boxart.cpp:1914](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/boxart.cpp#L1914)

```cpp
		hostfs::DirectoryTree artworkTree(artworkDir);
		for (auto it = artworkTree.begin(); it != artworkTree.end(); ++it)
		{
			const hostfs::FileInfo& entry = *it;
			if (!entry.isDirectory && isSupportedBoxartExtension(get_file_extension(entry.name)))
```

The early return currently follows both traversals. [core/ui/boxart/boxart.cpp:1929](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/boxart.cpp#L1929)

```cpp
	if (databasePaths.size() <= 1)
		return;

	const std::string mainDbPath = appendFilesystemPath(databaseDir, DB_NAME);
```

Recovery is entered when artwork and metadata directories coincide. [core/ui/boxart/boxart.cpp:2004](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/boxart.cpp#L2004)

```cpp
	if (databaseDir == artworkDir)
		recoverDatabases(databaseDir, artworkDir);
```

## Scope

- `core/ui/boxart/boxart.cpp`
- `core/ui/boxart/boxart.h`
- `tests/src/ui/BoxartTest.cpp`

Keep legacy database merging, normalized filename matching, duplicate score selection and atomic database publication. Custom media indexing and the offline absence cache serve different purposes. Isolate the recovery scan in measurements.

## Implementation and verification

### 1. Measure a populated cache before changing the algorithm

Add a repeatable `BoxartTest.PopulatedRecoveryWorkload` using the existing temporary data-root fixture. Populate a nested artwork tree with a fixed number of image entries and one actual database. Count recovery-specific tree scans/index builds using a narrow fixture observation, or instrument the scanner during the measurement. Print counts, dataset size and elapsed time. Keep this workload identical on baseline and candidate.

Run the **Populated workload** command below. The proposed workload runs five times and prints its recovery counts and elapsed time. Record the baseline before changing recovery.

### 2. Defer the image index until a merge is required

In `Boxart::recoverDatabases`, finish database discovery and return immediately for zero or one candidate. Only then construct `artworkTree`, normalize image names and allocate the index. Preserve the exception handling and fallback to normal database loading. For two or more databases retain the existing resolution, merge and save semantics.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Compare scan counts and recovery output

Run the identical workload five times for both versions on the same filesystem, recording warm-cache and first-process observations distinctly. In the named traversal-count test the single-database image index is never built. A duplicate legacy database fixture should build it once and repair a missing cover by name. Report timings as observations without a fixed timing threshold.

Run the **Focused** command below. The new RecoverySkipsImageIndexForSingleDatabase case is registered. The selected tests run and pass.

## Test plan

- Zero and one database both return after database discovery. Assert zero recovery image-index builds even with many nested image files.
- Two databases still merge with the same record keys, score preference and repaired image paths. Check corrupt secondary JSON preserves usable data and recovery does not delete source databases.
- Use the public cached-boxart lookup for at least one test so it covers first-use loading. Record independent directory/index counts for recovery so later custom-index work cannot mask the regression.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='BoxartTest.MovedFlycastDatabaseRecoversOnlyItsAdjacentDownloadedCover'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='BoxartTest.*' | rg -q '^[[:space:]]+RecoverySkipsImageIndexForSingleDatabase([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='BoxartTest.*'
```

The new RecoverySkipsImageIndexForSingleDatabase case is registered. The selected tests run and pass.

**Populated workload**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='BoxartTest.PopulatedRecoveryWorkload' | rg -q 'PopulatedRecoveryWorkload' && ./build-unit-tests-debug/Hollycast --gtest_filter='BoxartTest.PopulatedRecoveryWorkload' --gtest_repeat=5
```

The proposed workload runs five times and prints its recovery counts and elapsed time. Record the baseline before changing recovery.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] A normal populated library drops from two recursive recovery traversals to one, with zero image-index builds. Duplicate-database fixtures still recover their images. Record before/after dataset, counts and timing.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Accepted `0a84caa8a`. On both macOS 27.2.1 arm64 and Ubuntu 24.04 arm64, all 57 `BoxartTest` cases passed. The duplicate fixture performed two walks and built one index, recovered the preferred metadata and copied-name cover, and preserved both secondary databases, including the corrupt one. The diff passed the whitespace check and touched the three files listed in Scope.

The same public cached lookup ran five times per algorithm on each platform, using one database, 36 nested directories, and 240 images. The comparison moved the early return back below image indexing in the disposable review build. Both algorithms used identical instrumentation. Elapsed values measure recovery in microseconds. The first value is the first iteration of a new process.

| Host | Algorithm | Walks per lookup | Index builds per lookup | First | Subsequent four |
| --- | --- | --- | --- | --- | --- |
| macOS | Original | 2 | 1 | 7,971 | 8,217 / 5,895 / 6,236 / 5,896 |
| macOS | Reviewed | 1 | 0 | 3,009 | 3,303 / 3,026 / 2,942 / 3,421 |
| Ubuntu | Original | 2 | 1 | 633 | 618 / 671 / 697 / 718 |
| Ubuntu | Reviewed | 1 | 0 | 244 | 247 / 254 / 269 / 280 |

## Stop conditions

If the workload measures the independent custom media scanner or filesystem cache alone, isolate recovery-specific counts before making a startup-performance claim. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

The existing offline absent-art benchmark measures a different workload. Keep one populated recovery fixture when changing directory layout or metadata loading.
