# 003. Keep ALSA microphone results in 16-bit mono frames

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 3 | P1 | S | Low | Linux audio | `ebf8fbb79` · 2026-11-26 |

The Dreamcast microphone requests a count of mono samples. A short ALSA read currently zeros part of the valid prefix, and a device recovery status can become a sample count. Preserve recorded samples and always return a count in the requested range.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/alsa-microphone-frames`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/audio/audiobackend_alsa.cpp' \
  'core/audio/alsa_capture.h' \
  'tests/src/audio/AlsaCaptureTest.cpp' \
  'tests/CMakeLists.txt'
```

## Current behavior and local conventions

The capture backend mixes the prepare result, frame count and byte address. [core/audio/audiobackend_alsa.cpp:243](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/audio/audiobackend_alsa.cpp#L243)

```cpp
				DEBUG_LOG(AUDIO, "ALSA: Recording error: %s", snd_strerror(err));
				err = 0;
				err = snd_pcm_prepare(handle_record);
			}
			u8 *buffer = (u8 *)frame + err;
			memset(buffer, 0, (samples - err) * 2);
```

Maple supplies a byte array sized for 240 two-byte frames. [core/hw/maple/maple_devs.cpp:1001](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/hw/maple/maple_devs.cpp#L1001)

```cpp
				u8 micdata[240 * 2];
				u32 samples = RecordAudio(micdata, 240);
```

The SDL backend separately accounts for copied bytes and returned frames. [core/audio/audiobackend_sdl2.cpp:311](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/audio/audiobackend_sdl2.cpp#L311)

```cpp
			samples -= avail;
			count += avail;
		}
		DEBUG_LOG(AUDIO, "SDL2: sdl2_record len %d ret %d write %zd read %zd", samples * 2, count, (size_t)rec_write, (size_t)rec_read);

		return count / 2;
	}
};
```

## Scope

- `core/audio/audiobackend_alsa.cpp`
- `core/audio/alsa_capture.h` (new)
- `tests/src/audio/AlsaCaptureTest.cpp` (new)
- `tests/CMakeLists.txt`

Limit production changes to ALSA capture. Preserve the Maple packet format, playback, sample rates, SDL and CoreAudio. A narrow internal helper is appropriate if the tests need to exercise the same frame accounting without a microphone.

## Implementation and verification

### 1. Establish the Linux baseline

Use Linux with libasound development headers. Configure the unit preset and verify the actual compile command contains `-DUSE_ALSA`. Model the test as a small data-driven Google Test with fixed storage and a simulated read result.

Run the **ALSA build** command below. The project builds and ALSA was compiled in.

### 2. Separate counts from recovery

Keep `snd_pcm_readi` in its signed frame type until validated. Use a distinct recovery result. A transient no-data return yields zero frames and leaves the device available for the next request. Apply the backend’s recovery policy to genuine device errors. Clamp or reject an impossible count before arithmetic. Offset the byte buffer by `framesRead * sizeof(int16_t)` and clear only the unfilled tail. The caller’s byte array need not be suitably aligned for typed stores.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Verify through the capture implementation

Connect `record` to the tested accounting path. Add the named case and the error and boundary cases below. Where available, record a normal microphone sample on Linux and note device/backend details separately from unit results.

Run the **Focused** command below. The new ShortReadKeepsCapturedPrefix case is registered. The selected tests run and pass.

## Test plan

- For a short positive result, retain every byte in the captured prefix and zero the unused suffix. Check zero, one, odd, full and zero-capacity requests.
- Exercise the corrected normalization with no data, a successful device recovery and a failed recovery. All return zero frames for the failed read. The next successful read returns its own count.
- Use guard bytes on both sides of the fixture buffer and a simulated oversized count. The suite needs no audio device.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='MapleGgpoTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='AlsaCaptureTest.*' | rg -q '^[[:space:]]+ShortReadKeepsCapturedPrefix([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='AlsaCaptureTest.*'
```

The new ShortReadKeepsCapturedPrefix case is registered. The selected tests run and pass.

**ALSA build**

```sh
cmake --preset linux-unit-tests-debug -DUSE_ALSA=ON && cmake --build --preset linux-unit-tests-debug --parallel 4 && rg -q -- '-DUSE_ALSA' build-unit-tests-debug/compile_commands.json
```

The project builds and ALSA was compiled in.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The focused suite exercises the production count conversion, checks the complete prefix and suffix, and verifies that every returned count fits the request.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `e53aa8091`. Four frame-accounting tests passed on macOS and Ubuntu arm64. On Ubuntu 24.04, the registered ALSA backend read all 17 known mono samples from a file-backed capture device at 8,000 Hz and 11,025 Hz. A physical microphone was unavailable.

## Stop conditions

If the active Linux build omits ALSA, record the missing dependency before evaluating the backend. A passing SDL-only build is insufficient evidence. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Keep frame units in names around driver calls. Maple’s final packet rounding makes correct padding of short, odd-sized captures important.
