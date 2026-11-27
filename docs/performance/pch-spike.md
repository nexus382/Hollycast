# PCH and parallel CTest measurements

Measured 2026-11-27 on an Apple Silicon Mac (arm64, macOS 27.2) with Apple Clang 21.0.0, Ninja, Debug configuration, 10 logical CPUs, and 32 GiB RAM. Both build trees used `ENABLE_CTEST=ON`, `USE_VULKAN=OFF`, `USE_DISCORD=ON`, `USE_BREAKPAD=OFF`, and `FLYCAST_PRESEED_DARWIN_XCODE_CHECKS=OFF`; only `HOLLYCAST_PCH` differed. Builds used eight jobs. `ccache` was not installed. Build outputs were clean for each measurement, but the OS file cache was not flushed. Configure time is excluded.

## Full build

| Configuration | Build steps | Wall time | Actions |
| --- | --- | ---: | ---: |
| PCH off | `Hollycast` target, then remaining default targets | 16:20.41 + 24.539 s = 16:44.949 | 1,524 + 59 = 1,583 |
| PCH on | Clean default build | 7:29.76 | 1,583 |

The PCH reduced build wall time by 55.2%, above the 15% keep threshold. Use
`-DHOLLYCAST_PCH=ON` to opt in. CI and presets keep their existing compiler
configuration while Linux and the hosted macOS generator remain unverified.

The PCH includes common STL headers, `imgui.h`, and `core/types.h`. Header candidates were counted with:

```sh
grep -rh '^#include' core/ui/*.cpp | sort | uniq -c | sort -rn | head -20
```

That count identified `settings.h`, `gui.h`, and `cfg/option.h` as frequent first-party headers. A prototype with `settings.h` made `imgui_stdlib.cpp`'s `InputText` call ambiguous, so the smaller `core/types.h` was retained as the first-party header. The option applies to C++ translation units. Objective-C++ files can use different ARC settings.

The PCH-enabled full GoogleTest executable passed: 1,087 passed, 32 skipped, 3 disabled; exit code 0; 50.720 seconds. Optional skips require external game captures, shader packs, or explicit GPU test settings.

## CTest parallelism

The scoped command was `ctest --test-dir <build> --output-on-failure -R '^(hikaru_|updater_)' -E '^updater_linux_install$'`, which selected 22 tests on this host. The scan found no fixed HTTP listener ports; local HTTP tests bind ephemeral ports. The Hikaru group uses test backends, and updater cleanup uses a private build-tree location.

| Jobs | Run 1 | Run 2 |
| ---: | ---: | ---: |
| 1 (serial) | 53.255 s | — |
| 2 | 48.904 s | — |
| 4 | 37.245 s | 40.903 s |

All runs passed 22/22. Four jobs reduced wall time by 23–30% in this group.
The macOS PR test job uses four CTest jobs. Linux and Windows workflow
commands retain their existing scheduling until equivalent platform checks run.
