# 008. Reject incomplete Naomi ROM and BIOS reads

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 8 | P1 | M | Medium | Arcade loading | `ebf8fbb79` · 2026-11-26 |

Hollycast can find the correct archive member and still receive fewer bytes than the ROM descriptor requests. Mapping, interleaving or hashing the declared length then produces an invalid machine image. Report the affected component at load time and keep valid ROM sets and netplay identities compatible.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/naomi-rom-read-lengths`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/hw/naomi/naomi_cart.cpp' \
  'core/hw/naomi/naomi_cart.h' \
  'tests/src/hw/NaomiRomReadTest.cpp' \
  'tests/CMakeLists.txt'
```

## Current behavior and local conventions

The BIOS path can mark a region as loaded after a short read. [core/hw/naomi/naomi_cart.cpp:117](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/hw/naomi/naomi_cart.cpp#L117)

```cpp
				u32 read = file->Read(biosData + bios->blobs[romid].offset, bios->blobs[romid].length);
				if (config::GGPOEnable)
					md5.add(biosData + bios->blobs[romid].offset, bios->blobs[romid].length);
				DEBUG_LOG(NAOMI, "Mapped %s: %x bytes at %07x", bios->blobs[romid].filename, read, bios->blobs[romid].offset);
				found_region = true;
```

The interleaved path uses the declared length after reading into temporary storage. [core/hw/naomi/naomi_cart.cpp:370](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/hw/naomi/naomi_cart.cpp#L370)

```cpp
							u32 read = file->Read(buf, game->blobs[romid].length);
							u16 *to = (u16 *)CurrentCartridge->GetPtr(game->blobs[romid].offset, len);
							if (to == nullptr)
								throw NaomiCartException(strprintf(T("Invalid ROM: truncated %s"), game->blobs[romid].filename));
							u16 *from = (u16 *)buf;
							for (int i = game->blobs[romid].length / 2; --i >= 0; to++)
								*to++ = *from++;
```

The 7z adapter returns a bounded prefix and does not advance a cursor across calls. [core/archive/7zArchive.h:68](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/archive/7zArchive.h#L68)

```cpp
	u32 Read(void *buffer, u32 length) override
	{
		length = std::min(length, this->_length);
		memcpy(buffer, data + offset, length);
		return length;
```

Missing default EEPROM is explicitly optional. [core/hw/naomi/naomi_cart.cpp:342](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/hw/naomi/naomi_cart.cpp#L342)

```cpp
				if (!file) {
					WARN_LOG(NAOMI, "%s: Cannot open %s", fileName.c_str(), game->blobs[romid].filename);
					if (game->blobs[romid].blob_type != Eeprom)
						// Default eeprom file is optional
						throw NaomiCartException(strprintf(T("Cannot find %s"), game->blobs[romid].filename));
					else
						continue;
```

## Scope

- `core/hw/naomi/naomi_cart.cpp`
- `core/hw/naomi/naomi_cart.h`
- `tests/src/hw/NaomiRomReadTest.cpp` (new)
- `tests/CMakeLists.txt`

Use synthetic test data. Preserve CRC/name search order across child, parent and BIOS archives, game descriptor offsets, optional absent EEPROM, Copy blobs, and successful GGPO hash input order. Leave ZIP and 7z internals and the ROM databases intact.

## Implementation and verification

### 1. Define exact-read behavior at the cartridge boundary

Add a small, production-used Naomi read operation accepting an `ArchiveFile`, destination, required length and component name. Read the required slice once and compare for equality. A normal short return and an unsigned error return must both fail. Larger members can supply a legitimately requested prefix. Do not retry partial reads on the current 7z adapter.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Wire the BIOS and cartridge cases

Use the operation for BIOS Normal and EepromBE16 and game Normal, InterleavedWord, Key and Eeprom, including the 0x84 serial case. For temporary EEPROM/key buffers use ownership compatible with their existing consumer and publish only after success. Avoid hashing, byte swapping, changing the selected region or exposing a key before the read succeeds. For direct mapped bytes rely on verified disposal of a failed cartridge or stage the component if the target outlives the failure.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Exercise cleanup and compatibility

Register the named regression. Drive a fake archive through the relevant mapping code, assert the error names the component and the load does not publish a digest or leave an active partial cartridge. Relaunch with the complete data and compare interleaved output and successful hash bytes with the original layout.

Run the **Focused** command below. The new ShortMemberIsRejectedBeforePublish case is registered. The selected tests run and pass.

## Test plan

- Read exact and larger-than-required members, short/zero/error returns, and optional EEPROM absent versus present but incomplete. Include a successful synthetic key and serial EEPROM case.
- Use distinct small words in adjacent interleaved lanes. Assert byte order and lane spacing on success. Verify the fake reader is called only once per required slice.
- Fit the final high lane exactly against the cartridge boundary, as in Giant Gram 2000. Verify both lanes and the existing GGPO hash inputs.
- Check the existing error cleanup resets the failed cartridge and a fresh valid load succeeds. No real BIOS archive or network peer is needed.
- Use the production loader for a multi-member cartridge failure followed by a good retry. Check the active cartridge, inputs, serial, default EEPROM and digest. For BIOS loading, confirm that an incomplete EEPROM following a complete program leaves the previously selected BIOS and region intact.

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
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='NaomiRomReadTest.*' | rg -q '^[[:space:]]+ShortMemberIsRejectedBeforePublish([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='NaomiRomReadTest.*'
```

The new ShortMemberIsRejectedBeforePublish case is registered. The selected tests run and pass.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The exact-read tests cover the actual loading path, including no publication after failure and a stable digest on success. Every relevant `ArchiveFile::Read` in the BIOS and cartridge switches passes through validation.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `f10d12e13`. Ten synthetic archive tests and the Dreamcast header compatibility test passed on macOS and Ubuntu arm64. The tests cover both interleave lanes at the cartridge boundary, short required reads, optional EEPROMs, BIOS word order, publication after success, cleanup and retry.

## Stop conditions

If a descriptor intentionally consumes only a prefix, validate the requested prefix. If a real set needs a shorter-than-declared member, identify that descriptor and its documented fill rule before adding an exception. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

`GetPtr` validates access to cartridge space. It does not verify bytes received from an archive. Keep those responsibilities distinct when adding blob types.
