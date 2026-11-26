# 017. Describe Hikaru defaults and graphics backends accurately

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 17 | P3 | S | Low | Player documentation | `ebf8fbb79` · 2026-11-26 |

The main README tells players that Hikaru is disabled by default and describes it as a desktop OpenGL backend. Eligible standalone builds already enable it on four host platforms, and several renderer paths exist. Readers should be able to tell whether they need to rebuild at all.

## Before starting

Work from the Hollycast repository root. Branch from `dev` as `docs/hikaru-build-defaults`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'README.md'
```

## Current behavior and local conventions

Replace this introductory paragraph and preserve its link to the compatibility guide. [README.md:57](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/README.md#L57)

```markdown
An optional desktop OpenGL backend adds experimental **Sega Hikaru / Air Trix**
support. It is disabled by default and still has graphics and audio limitations.
See [Hikaru setup and compatibility](docs/Hikaru.md) before enabling `USE_HIKARU`.
```

CMake sets the default only for eligible standalone targets with the required build capabilities. [CMakeLists.txt:105](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/CMakeLists.txt#L105)

```cmake
set(USE_HIKARU_DEFAULT OFF)
if(NOT LIBRETRO AND NOT WINDOWS_STORE AND NOT WEBOS AND NOT IOS AND NOT NINTENDO_SWITCH
   AND USE_OPENGL AND NOT USE_VIDEOCORE AND NOT USE_GLES2
   AND (WIN32 OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
	set(USE_HIKARU_DEFAULT ON)
endif()
option(USE_HIKARU "Build the experimental Sega Hikaru backend" ${USE_HIKARU_DEFAULT})
```

The platform guide lists Windows, Android, Linux and macOS. [docs/Hikaru.md:166](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/docs/Hikaru.md#L166)

```markdown
Hikaru is enabled by default in standalone Windows, Android, Linux and macOS
builds. Other desktop hosts can opt in if their graphics requirements are met;
those configurations are unverified. It can be forced on or off at configure time:
```

Use the renderer names and hardware qualifications in the platform guide. [docs/Hikaru.md:180](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/docs/Hikaru.md#L180)

```markdown
Hikaru supports desktop OpenGL 3.2, OpenGL ES 3.0 on Android, and native
Vulkan, Metal or Direct3D 11 where those backends are available. On Windows,
choose DirectX 11 for the native Direct3D path. DirectX 9 and OpenGL ES 2
cannot run Hikaru. The Advanced **Multi-threading** setting enables Hikaru's
```

## Scope

- `README.md`

Edit only the Hikaru overview paragraph in the main README. Keep policy in CMake and detailed title compatibility in the dedicated guide. The configure-time OpenGL requirement and the selected runtime renderer are distinct.

## Implementation and verification

### 1. Check the source of truth

Read the `USE_HIKARU_DEFAULT` predicate and its fatal guards together with the guide’s build and run section. Preserve the qualifiers for standalone builds and for backend availability. The game list in the guide describes recognized titles and individual compatibility levels.

Run the **Source facts** command below. The default predicate and the guide agree on qualified hosts and renderers.

### 2. Replace the stale overview

Say experimental Sega Hikaru support is enabled by default on eligible standalone Windows, Android, Linux and macOS builds. Describe runtime options compactly using OpenGL/OpenGL ES, Vulkan, Metal and Direct3D 11 where supported, or point directly to the guide for platform details. Keep a brief experimental-status qualification and the local guide link. Avoid expanding the paragraph into a per-title support matrix.

Run the **Overview** command below. The overview names the eligible hosts and experimental status, contains the live guide link, and drops the contradictory default.

### 3. Check the rendered wording and link

Read the edited paragraph in context with the surrounding feature list. Run the scope check to confirm only this paragraph changed. A build is unnecessary for this README-only change.

Run the **Scope** command below. Both whitespace checks exit zero. Compare the change list with the scope below.

## Test plan

- Read the default from CMake with its platform and build-option guards. In the prose, qualify the platform list with “eligible standalone builds.”
- Verify the relative compatibility link resolves to a tracked file. Avoid implying universal title playability or support for every renderer on every platform.

## Verification commands

**Source facts**

```sh
rg -n 'USE_HIKARU_DEFAULT|USE_HIKARU requires standalone' CMakeLists.txt && rg -n 'Hikaru is enabled by default|Hikaru supports desktop OpenGL|Vulkan, Metal or Direct3D 11' docs/Hikaru.md
```

The default predicate and the guide agree on qualified hosts and renderers.

**Overview**

```sh
python3 -B - <<'PY'
from pathlib import Path
import re
root = Path("README.md").read_text()
intro = root.split("Everything Flycast offers", 1)[1].split("## Why a fork?", 1)[0]
text = " ".join(intro.lower().split())
for term in ("hikaru", "experimental", "standalone", "windows", "android", "linux", "macos", "default"):
    assert term in text, term
assert "disabled by default" not in text
link = re.search(r'\[[^\]]+\]\((docs/Hikaru\.md)\)', intro)
assert link and Path(link.group(1)).is_file()
PY
```

The overview names the eligible hosts and experimental status, contains the live guide link, and drops the contradictory default.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The Overview and Source facts commands pass. The scoped README diff correctly reflects the current build predicate and links to the runtime renderer guide.
- [x] The documentation and tracked-link checks passed on macOS arm64.
- [x] The reviewed diff changes one README paragraph. The [plan index](README.md) records the accepted commit.

## Independent review

Reviewed `7a4f3ba0c` against the CMake defaults and compatibility guide. The four eligible platforms and renderer choices match the source. The overview links to the tracked guide and identifies the known graphics and audio limitations. The scoped README diff changes one paragraph.

## Stop conditions

If CMake or the guide has changed since the baseline and they now disagree, establish the actual configured default before updating documentation. Keep any build-policy proposal separate. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Update this paragraph alongside changes to `USE_HIKARU_DEFAULT` or the supported host matrix. Let the detailed guide own renderer requirements and game-specific results.
