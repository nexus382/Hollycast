# 010. Calculate CUE offsets from each track’s physical sector width

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 10 | P2 | M | Medium | Disc images | `ebf8fbb79` · 2026-11-26 |

A single BIN can contain tracks with different physical record sizes. Multiplying the INDEX frame number by the later track’s sector size skips the wrong number of bytes. That can change play data, audio and the advertised lead-out even though the file opens.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/mixed-width-cue-tracks`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/imgread/cue.cpp' \
  'core/imgread/common.h' \
  'tests/src/imgread/CueTest.cpp'
```

## Current behavior and local conventions

The parser currently calculates the entire FILE length using its first track’s width. [core/imgread/cue.cpp:228](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/imgread/cue.cpp#L228)

```cpp
			// Can happen if multiple tracks with different sector sizes share the same file
			if (fileInfo.size % track_secsize != 0)
				WARN_LOG(GDROM, "Warning: Size of track %s is not multiple of sector size %d", track_filename.c_str(), track_secsize);
			// FIXME This is wrong if tracks of different sector size share the same file
			if (firstTrackOfFile)
				currentFAD += fileInfo.size / track_secsize;
```

INDEX 01 creates a RawTrackFile from one sector width and an absolute frame position in the BIN. [core/imgread/cue.cpp:269](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/imgread/cue.cpp#L269)

```cpp
				hostfs::File *track_file = hostfs::storage().openFile(track_filename, "rb");
				if (track_file == nullptr)
					throw FlycastException(strprintf(i18n::T("CUE file: cannot open track %s"), track_filename.c_str()));
				t.file = new RawTrackFile(track_file, indexFAD * track_secsize, t.StartFAD, track_secsize);
				disc->tracks.push_back(t);
```

RawTrackFile converts the byte position and first logical FAD into a seek offset. [core/imgread/common.h:221](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/imgread/common.h#L221)

```cpp
	RawTrackFile(hostfs::File *file, u32 file_offs, u32 first_fad, u32 secfmt)
	{
		verify(file != nullptr);
		this->file = file;
		this->offset = file_offs - first_fad * secfmt;
		this->fmt = secfmt;
```

Existing tests check logical track starts. Extend them to inspect known bytes at the boundary. [tests/src/imgread/CueTest.cpp:15](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/src/imgread/CueTest.cpp#L15)

```cpp
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_FALSE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(600, disk->tracks[1].StartFAD);
```

## Scope

- `core/imgread/cue.cpp`
- `core/imgread/common.h`
- `tests/src/imgread/CueTest.cpp`

Limit parsing changes to CUE file layout and any checked width calculation required by RawTrackFile. Preserve separate-BIN, uniform-width, CDI/CHD/GDI, multisession, GD-ROM, ISRC and digest behavior. Generate fixture data during tests.

## Implementation and verification

### 1. Build a tiny mixed-width disc fixture

Construct a temporary CUE and BIN with three 2048-byte Mode 1 frames followed by two 2352-byte audio frames. Give every sector a distinct pattern. With an INDEX 01 of 00:00:03 the audio starts at byte 6144, logical FAD 153 from the usual base of 150. Verify the last data sector and first audio sector through the normal disc reader.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Accumulate physical byte ranges per FILE

Collect the current FILE’s track/index records or defer its end until the next physical boundary. Accumulate each span using the width in force for that span. Compute the tail length from remaining bytes and the final width, then derive per-track start/end, next FILE start and lead-out. Account for stored INDEX 00 pregap bytes with the incoming width and keep synthesized PREGAP sectors out of the file offset. Handle EOF, FILE, session and density transitions exactly once.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Protect arithmetic and verify compatibility

Use checked 64-bit byte arithmetic for physical offsets and reject impossible boundaries before opening tracks. If the shared RawTrackFile offset narrows a valid result, widen its offset and seek arithmetic and run existing raw-track consumers. Keep the signed relationship between physical offset and logical FAD. Add the reverse-width, multiple-FILE and session cases below.

Run the **Focused** command below. The new ReadsFirstSectorAfterMixedWidthBoundary case is registered. The selected tests run and pass.

## Test plan

- Read sentinel bytes at both sides of each boundary, check sector format and track starts, and assert lead-out. Add a longer case where dividing total bytes by the first width gives the wrong number of frames.
- Exercise raw audio followed by cooked data, a new FILE after a mixed FILE, stored `INDEX 00` and synthetic `PREGAP`. Include uniform-width audio and existing multi-session/GD fixtures as controls.
- Verify monotonic and out-of-range indices produce a normal parse failure. Open the same fixture with the optional digest and check that each physical track file contributes once as before.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='CueTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='CueTest.*' | rg -q '^[[:space:]]+ReadsFirstSectorAfterMixedWidthBoundary([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='CueTest.*'
```

The new ReadsFirstSectorAfterMixedWidthBoundary case is registered. The selected tests run and pass.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named regression reads the first audio sector from the expected byte offset. All existing CueTest cases and the new sector, duration, pregap and lead-out assertions pass.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `20abca75`. Nine CUE cases, four GDI cases and an independent public Disc::ReadSectors test passed on macOS and Ubuntu arm64. Both stored audio pregap sectors returned all 2,352 expected bytes. Synthetic gaps and TOC positions passed. A MinGW x64 executable under Wine measured a 3,221,225,492-byte sparse file and read the marker at byte 3,221,225,489. The previous implementation returned a negative size and failed the seek.

## Stop conditions

If a previously accepted unusual pregap layout changes, retain a minimal fixture showing its prior TOC and bytes before choosing a compatibility rule. Do not infer missing on-disk bytes from synthesized silence. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Keep three quantities explicit in names and tests: file-relative frame index, physical byte offset and emulated FAD. Finalizing a FILE also sets the base for the next FILE.
