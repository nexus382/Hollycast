# 016. Pin the Switch SDK image and partition its compiler cache

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 16 | P3 | S | Low | Build dependencies | `ebf8fbb79` · 2026-11-26 |

The Switch matrix resolves the latest SDK on every dispatch. Two runs of the same source can therefore use different compilers. Pin the image actually validated for both standalone and libretro, and make a toolchain change select a new ccache namespace.

## Before starting

Work from the Hollycast repository root. Branch from `dev` as `ci/pin-switch-sdk`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  '.github/workflows/switch.yml'
```

## Current behavior and local conventions

Both matrix jobs inherit one unpinned devkitPro container. [.github/workflows/switch.yml:9](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/.github/workflows/switch.yml#L9)

```yaml
    name: Switch ${{ matrix.config.name }}
    runs-on: ubuntu-latest
    container: devkitpro/devkita64:latest
```

CI packages standalone and libretro separately. [.github/workflows/switch.yml:18](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/.github/workflows/switch.yml#L18)

```yaml
        config:
          - { name: standalone, destDir: switch }
          - { name: libretro, cmakeArgs: -DLIBRETRO=ON }
```

The current key and restore prefix separate build modes but omit the SDK identity. [.github/workflows/switch.yml:36](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/.github/workflows/switch.yml#L36)

```yaml
      - uses: actions/cache@v6
        with:
          path: ${{ env.CCACHE_DIR }}
          key: ccache-switch-${{ matrix.config.name }}-${{ github.sha }}
          restore-keys: ccache-switch-${{ matrix.config.name }}-
```

The install target puts the NRO and libretro output in different destinations. [CMakeLists.txt:1674](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/CMakeLists.txt#L1674)

```cmake
elseif(NINTENDO_SWITCH AND NOT LIBRETRO)
	install(FILES ${CMAKE_BINARY_DIR}/Hollycast.nro DESTINATION "${CMAKE_INSTALL_BINDIR}")
elseif(LIBRETRO)
	install(TARGETS ${PROJECT_NAME} DESTINATION "${CMAKE_INSTALL_LIBDIR}/libretro")
```

## Scope

- `.github/workflows/switch.yml`

Change the SDK reference, ccache namespace and a short in-workflow bump comment. Preserve the existing runner, matrix, packaging, artifact names, publication conditions and toolchain command. Pinning the SDK does not freeze apt or other Actions dependencies.

## Implementation and verification

### 1. Identify a verifiable SDK candidate

Prefer a digest recorded in the logs of a successful Switch matrix. If unavailable, inspect the official devkitpro/devkita64 image to select a candidate at execution time. Record the immutable manifest digest, linux/amd64 availability and compiler version. A moving tag is only a discovery input.

Run the **Inspect candidate** command below. The registry reports the manifest digest and image metadata. Inspect the selected immutable reference again to confirm it resolves.

### 2. Pin the image and cache namespace

Use `devkitpro/devkita64@sha256:<verified digest>` in the container field. Include both the matrix variant and a fingerprint that changes with that pin in the exact key and restore prefix. A hash of the checked-out workflow is sufficient and keeps the digest in one place. Add a short comment giving the inspect and two-variant verification procedure.

Run the **Workflow** command below. The workflow lints, the pin is a full digest, and both cache lines show the same toolchain-scoped prefix.

### 3. Build both variants from the immutable image

Use a clean host checkout with pinned submodules. Run the two install builds below using the image parsed back from the edited workflow. Capture the compiler identity and confirm both install artifacts are nonempty. This local check leaves output in the disposable container. CI cache behavior can also be verified from the next authorized run.

Run the **Both variants** command below. The digest resolves, the compiler reports its version, both builds exit zero, and each install contains the expected nonempty product.

## Test plan

- Validate the digest with the registry and both matrix variants using the same reference. Quote the tested digest and compiler version in the implementation record.
- Use the existing separate matrix labels in both cache keys. Changing the image pin must change the restore prefix for both builds. A later change of source SHA on the same toolchain may reuse its variant’s cache.
- Use the release workflow’s install layout to inspect the standalone NRO and libretro product. Report build reproducibility at SDK level only.

## Verification commands

**Inspect candidate**

```sh
docker buildx imagetools inspect devkitpro/devkita64:latest
```

The registry reports the manifest digest and image metadata. Inspect the selected immutable reference again to confirm it resolves.

**Workflow**

```sh
actionlint -shellcheck= .github/workflows/switch.yml &&
python3 -B - <<'PY'
import re
from pathlib import Path
workflow = Path(".github/workflows/switch.yml").read_text()
assert re.search(r"^\s*container:\s*devkitpro/devkita64@sha256:[0-9a-f]{64}\s*$", workflow, re.M)
key = re.search(r"^\s*key:\s*(.+)$", workflow, re.M).group(1)
prefix = re.search(r"^\s*restore-keys:\s*(.+)$", workflow, re.M).group(1)
assert key.startswith(prefix) and "github.sha" in key[len(prefix):]
assert "matrix.config.name" in prefix
assert "hashFiles('.github/workflows/switch.yml')" in prefix
PY
```

The Actions definition lints. The check verifies a full digest and a common variant- and workflow-scoped cache prefix. The lint command checks the edited YAML fields. The baseline shell scripts already produce unrelated SC2086 messages with ShellCheck enabled.

**Both variants**

```sh
sdk_image=$(awk '/^[[:space:]]*container:/ {print $2; exit}' .github/workflows/switch.yml)
docker buildx imagetools inspect "$sdk_image" &&
docker run --rm --platform linux/amd64 --mount "type=bind,src=$PWD,dst=/source,readonly" "$sdk_image" bash -eu -c '
  for variant in standalone libretro; do
    retro=OFF
    if [ "$variant" = libretro ]; then retro=ON; fi
    "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" -S /source -B "/tmp/build-$variant" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="/tmp/install-$variant" -DLIBRETRO="$retro" -DHOLLYCAST_REQUIRE_SCREENSCRAPER=OFF
    compiler=$(sed -n "s/^CMAKE_CXX_COMPILER:[^=]*=//p" "/tmp/build-$variant/CMakeCache.txt")
    "$compiler" --version
    cmake --build "/tmp/build-$variant" --config Release --target install
  done
  test -s /tmp/install-standalone/bin/Hollycast.nro
  find /tmp/install-libretro -type f -path "*/libretro/*" -size +0c -print -quit | grep -q .
'
```

The digest resolves, the compiler reports its version, both builds exit zero, and each install contains the expected nonempty product.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The workflow uses one verified immutable image reference. Both variants build with it and produce artifacts. The exact key and restore prefix both change after a toolchain bump.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `0f609276c`. Actionlint and the cache-key change check passed. A network-disabled linux/amd64 container at the pinned devkitA64 digest built and installed both Release variants with GCC 15.2.0. The original toolchain check produced a 64 MiB NRO and a 227 MiB libretro archive. The combined source also built both targets, producing a 64 MiB NRO and a 229 MiB libretro archive.

## Stop conditions

If the registry cannot supply the intended platform or one product fails on the candidate SDK, keep the last verified digest or leave pin selection pending until both builds succeed. Do not substitute an unverified digest. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

For each SDK upgrade record the prior reference, candidate digest, compiler version and outcomes for both variants. The cache fingerprint should invalidate automatically when the pin changes.
