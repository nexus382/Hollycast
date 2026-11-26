# 014. Persist LaunchBox match updates without rewriting the inventory per game

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 14 | P2 | M | Medium | Artwork performance | `ebf8fbb79` · 2026-11-26 |

Every changed match serializes the entire map. A bulk import of N fresh matches can serialize 1 + 2 + … + N records. Preserve recoverable progress, especially after a provider error, while reducing bytes written across a complete inventory.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `perf/launchbox-match-writes`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/ui/boxart/launchbox.cpp' \
  'core/ui/boxart/launchbox.h' \
  'core/ui/boxart/scraper.h' \
  'core/ui/boxart/artwork_adapter.cpp' \
  'core/ui/boxart/artwork_adapter.h' \
  'core/ui/boxart/boxart.cpp' \
  'tests/src/ui/LaunchBoxTest.cpp' \
  'tests/src/ui/ArtworkAdapterTest.cpp' \
  'tests/src/ui/ScreenScraperTest.cpp' \
  'tests/src/ui/BoxartTest.cpp'
```

## Current behavior and local conventions

The v2 snapshot contains the full matches map. [core/ui/boxart/launchbox.cpp:415](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/launchbox.cpp#L415)

```cpp
void LaunchBox::saveMatches()
{
	writeJson(directory + "launchbox-matches.json", {{"version", 2}, {"matches", matches}});
}
```

A successful changed item triggers a whole-map save. [core/ui/boxart/launchbox.cpp:529](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/launchbox.cpp#L529)

```cpp
 if (matchesChanged)
 {
  try { saveMatches(); }
  catch (const std::exception&)
```

Startup accepts both v1 and v2 snapshots. [core/ui/boxart/launchbox.cpp:375](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/launchbox.cpp#L375)

```cpp
		const json saved = readJson(directory + "launchbox-matches.json");
		if (saved.is_object() && (saved.value("version", 0) == 1 || saved.value("version", 0) == 2) && saved.contains("matches") && saved["matches"].is_object())
			matches = saved["matches"];
```

One-item calls already guarantee a readable, complete match after the final requested image. [tests/src/ui/LaunchBoxTest.cpp:314](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/src/ui/LaunchBoxTest.cpp#L314)

```cpp
 EXPECT_FALSE(matchCachePresentDuringLaterImage);

 std::ifstream cache(root / "launchbox-matches.json");
 ASSERT_TRUE(cache);
 const json saved = json::parse(cache);
 const auto& cachedMedia = saved.at("matches").at(LaunchBoxCatalog::identity(item)).at("media");
 EXPECT_EQ(10u, cachedMedia.size());
```

## Scope

- `core/ui/boxart/launchbox.cpp`
- `core/ui/boxart/launchbox.h`
- `core/ui/boxart/scraper.h`
- `core/ui/boxart/artwork_adapter.cpp`
- `core/ui/boxart/artwork_adapter.h`
- `core/ui/boxart/boxart.cpp`
- `tests/src/ui/LaunchBoxTest.cpp`
- `tests/src/ui/ArtworkAdapterTest.cpp`
- `tests/src/ui/ScreenScraperTest.cpp`
- `tests/src/ui/BoxartTest.cpp`

Keep artwork selection, matching identity, catalog download, existing snapshots and single-item scrape semantics. Measure match-cache I/O independently of image transfers, catalogue initialization and the general boxart database.

## Implementation and verification

### 1. Measure real serialization with an offline inventory

Extend `LaunchBoxTest` with `LaunchBoxMediaTest.BulkMatchWriteWorkload`. Use a local mock catalog and deterministic equal-sized records for 128 and 256 distinct identities. Count records serialized across snapshots and updates, bytes written, number of physical writes and elapsed time at the production persistence boundary. Run baseline before changing the writer.

Run the **Bulk workload** command below. Both dataset sizes complete offline and print serialization counts and bytes. Capture the original and candidate results.

### 2. Give bulk work a recoverable incremental path

Keep standalone `scrape(item)` immediately readable. During an explicit bulk interval, persist each completed or partially completed changed identity once as a small durable update, then compact to the existing v2 snapshot at drain. A small keyed update log beside the snapshot is one option. Replay updates over a v1/v2 base on initialize. Use idempotent records and an atomic snapshot replacement before retiring updates. Recover an interrupted trailing write without losing earlier commits or future updates. Propagate persistence failures because `writeJson` currently only logs or returns.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Connect the actual queue owners

Trace both the bulk `ArtworkAdapter` path and the serial LaunchBox fallback inside `runScreenScraperQueue`. Give the provider a real start/drain boundary spanning queue refills, and flush pending work on cancellation, failure or shutdown. Serialize per-directory writers if provider instances can overlap. Preserve the current direct-call partial-failure tests and avoid making a fixed-size batch repeatedly serialize a still-growing full map.

Run the **Focused** command below. The new BulkMatchesSurviveRestartWithoutPerItemSnapshots case is registered. The selected tests run and pass.

### 4. Compare scale and crash recovery

Repeat the identical workload with clean temporary profiles. For one uninterrupted inventory, check a linear bound on records serialized, such as one delta per changed record plus at most one full snapshot at drain and any fixed startup overhead. Test restart from an unfinished interval. Put the observed counts and bytes alongside elapsed time in the change description.

Run the **Bulk workload** command below. Both dataset sizes complete offline and print serialization counts and bytes. Capture the original and candidate results.

## Test plan

- Cover v1 upgrade, v2 reuse, a renamed disc, partial media failure, and replay after process interruption before and after snapshot publication. Latest completed values win for duplicate identities.
- Exercise cache write failure and a clean retry, cancellation, queue refills and recreation of the provider. Check direct one-item calls still expose the complete v2 snapshot at the same boundary as today.
- Count actual serialized records including compaction. Doubling the modeled inventory should approximately double work after the change. Use deterministic record counts for assertions and retain timing as measured evidence.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='LaunchBoxCacheTest.RestoresCoverForRenamedDiscWithoutCatalogOrNetwork:LaunchBoxMediaTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='LaunchBoxCacheTest.*:LaunchBoxMediaTest.*:ScreenScraperTest.*' | rg -q '^[[:space:]]+BulkMatchesSurviveRestartWithoutPerItemSnapshots([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='LaunchBoxCacheTest.*:LaunchBoxMediaTest.*:ScreenScraperTest.*'
```

The new BulkMatchesSurviveRestartWithoutPerItemSnapshots case is registered. The selected tests run and pass.

**Bulk workload**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='LaunchBoxMediaTest.BulkMatchWriteWorkload' | rg -q 'BulkMatchWriteWorkload' && ./build-unit-tests-debug/Hollycast --gtest_filter='LaunchBoxMediaTest.BulkMatchWriteWorkload' --gtest_repeat=3
```

Both dataset sizes complete offline and print serialization counts and bytes. Capture the original and candidate results.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named test recovers all completed matches after restart. Existing direct-call tests pass. The 128/256-record workload records lower byte counts and satisfies the linear record bound for a single bulk interval.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `78de0dda`. The 123 offline artwork and queue checks passed on both macOS and Ubuntu arm64. A cached rescan performed zero writes, and the next provider completed compaction after an interrupted snapshot. Both platforms read the public metadata export. macOS also downloaded and reopened a real cover. A Windows x64 test under Wine replayed a journal with a 9,008-byte torn tail in a Japanese directory.


The independent workload used the same catalogue, distinct serials and cache directory layout before and after the change. On the original writer each completed item published the full snapshot. The reviewed writer appended one update per item and published one full snapshot at drain. Counts and bytes came from the files on disk. Timings include the regression's disk reads and depend on the host. Temporary directory names are serialized in records, so their lengths affect byte totals.

| Host | Matches | Original records | Reviewed records | Original bytes | Reviewed bytes | Original elapsed | Reviewed elapsed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| macOS arm64 | 128 | 8,256 | 256 | 8,217,920 | 258,620 | 759 ms | 57 ms |
| macOS arm64 | 256 | 32,896 | 512 | 32,737,920 | 517,308 | 3,041 ms | 111 ms |
| Ubuntu arm64 | 128 | 8,256 | 256 | 7,821,632 | 246,076 | 248 ms | 28 ms |
| Ubuntu arm64 | 256 | 32,896 | 512 | 31,158,912 | 492,220 | 1,030 ms | 63 ms |

The public export used for compatibility was published on 26 November 2026 and contained a 525,667,616-byte Metadata.xml. The ZIP measured 110,961,244 bytes with SHA-256 `ad0601ccf618dbed0b231c7e1f9dee465d563688c33a80842787a38b3903fe05`. The moving export and cover are optional tests. The bulk measurements use a fixed offline fixture.

## Stop conditions

If the queue does not expose a reliable bulk lifetime, move the aggregation boundary to the actual inventory owner before deferring snapshots. If deterministic I/O does not fall, retain the baseline and revisit the strategy before adopting a new cache format. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Preserve replay ordering across interruption and flush partial provider progress on error. Batching a fixed number of items per full snapshot still produces quadratic total serialization.
