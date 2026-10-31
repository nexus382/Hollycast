<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast

![Hollycast](banner.png)

<sub>Logo credit: [@ruva](https://artistree.io/ruva)</sub>

[![Latest release](https://img.shields.io/github/v/release/nexus382/Hollycast?label=release)](https://github.com/nexus382/Hollycast/releases/latest)
[![License: GPL v2](https://img.shields.io/badge/license-GPLv2-blue.svg)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/8s8Ecc2BxR)
[![Website](https://img.shields.io/badge/web-hollycast.org-orange)](https://hollycast.org)

**Hollycast is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator.** It is built on [Flycast](https://github.com/flyinghead/flycast): it follows the upstream emulation core and adds a modern interface, more front-end features, and controls for power users.

- [Download](#download)
- [What Hollycast adds](#what-hollycast-adds)
- [Why a fork?](#why-a-fork)
- [Contributing](#contributing)
- [Building from source](#building-from-source)
- [Debugging](#debugging)
- [Support the project](#support-the-project)
- [License and credits](#license-and-credits)

## Download

Prebuilt binaries for every platform are on the **[latest release page](https://github.com/nexus382/Hollycast/releases/latest)**. Each release includes a `SHA256SUMS.txt` file so you can check your download.

| Platform | Release file |
| --- | --- |
| Windows (x64) | `Hollycast-<version>-windows-x64.zip`, or the MSVC builds `windows-msvc-x64.zip` / `windows-msvc-x86.zip` |
| macOS (Intel and Apple Silicon) | `Hollycast-<version>-macos-universal.zip` |
| Linux (x64) | `Hollycast-<version>-linux-x64.AppImage` |
| Android | `Hollycast-<version>-android.apk` |
| Xbox / UWP | `Hollycast-<version>-uwp-x64.appx` (also `x86` and `arm64`). Install steps are in `UWP-INSTALL.txt` on the release page. |
| Nintendo Switch | `Hollycast-<version>-switch.nro` |
| RetroArch (libretro core) | `windows-libretro-x64.zip`, `linux-libretro-x64.zip`, `linux-libretro-arm64.zip`, `switch-libretro.zip` |

Releases tagged `-dev` are built from the `dev` branch and contain the newest, less-tested changes. Other releases are built from `master`.

## What Hollycast adds

- **Modern interface.** A redesigned UI with a game library that shows box art and VMU icons.
- **Continue Playing and favourites.** Continue Playing lists every game with a save state, newest first, and Resume loads that title's newest manual state or autosave. Start Fresh boots without loading a state. Use the star on a game to keep it in Favourites. These choices do not change your automatic save-state settings.
- **Library theme packages.** Install themes with custom palettes and library layouts, including Art Book Next, Iconic and Linear ports. See the [theme author guide](docs/Themes.md).
- **Ready to play.** Sensible defaults let new players start a game straight away, and the advanced settings are still there for power users.
- **Performance overlay.** A configurable on-screen display with frame timing plus RAM and texture VRAM usage.
- **Real hardware and VMU support.** [DreamLink](docs/DreamLink.md) connects Hollycast to DreamConn and DreamPicoPort controllers and to the [DreamPotato](docs/DreamPotato%20Integrated%20Mode.md) VMU emulator.
- **Editable virtual controller.** The on-screen touch controls can be rearranged and resized on mobile.
- **Frequent quality-of-life updates.** Old components are kept up to date, and new features come from requests by people who play every day.

Everything Flycast offers is still included: the full Dreamcast, Naomi, Naomi 2 and Atomiswave core, RetroAchievements, upscaling and widescreen options, and the libretro core.

## Why a fork?

Hollycast follows the Flycast core and is a home for features and experiments that are outside Flycast's scope:

- **More features.** It includes features the community asked for and experimental changes that Flycast chose not to merge.
- **Room to experiment.** It gives developers a stable place to build features they care about and to modernize the emulator.

Its technical standards are the same as Flycast's:

- **Performance parity.** The aim is to match or beat the speed of the current Flycast core.
- **Verified accuracy.** Flycast's own test suite runs alongside a growing set of Hollycast tests to catch regressions.
- **Core integrity.** New features are built so that they don't make the core less stable.

Hollycast aims to support every platform and release that Flycast supports. Merging Flycast updates gets harder as the two projects drift apart, so changes from upstream are brought in when they fit Hollycast and don't cause regressions.

## Contributing

Hollycast is under active development. If you'd like to contribute, please [join our Discord](https://discord.gg/8s8Ecc2BxR) and open a feature request to discuss your plans **before** you write code.

Bugs can be reported with the [bug report template](https://github.com/nexus382/Hollycast/issues/new?template=bug_report.md).

## Building from source

### 1. Install prerequisites

<details>
<summary><b>Windows</b></summary>

1. Install Visual Studio with MSVC and the C++ CMake tools. They are in the `Desktop development with C++` workload of the installer.
2. Add the folder that contains `cmake.exe` to your `PATH`, for example `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`.
3. *Android builds only:* turn on `Developer Mode` under **System > Advanced** in Windows Settings so that symlinks can be created without administrator rights. **Warning:** this setting is generally considered a [vulnerability on Windows](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/create-symbolic-links#vulnerability).

</details>

<details>
<summary><b>Linux (Debian / Ubuntu)</b></summary>

```bash
sudo apt-get update
sudo apt-get -y install git cmake gcc g++
sudo apt-get -y install ccache libao-dev libasound2-dev libevdev-dev libgl1-mesa-dev liblua5.3-dev libminiupnpc-dev libpulse-dev libsdl2-dev libudev-dev libzip-dev ninja-build libcurl4-openssl-dev libcdio-dev libfuse2 locales
sudo apt-get -y install libwayland-dev libdecor-0-dev libaudio-dev libjack-dev libsndio-dev libsamplerate0-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev fcitx-libs-dev
```

</details>

<details>
<summary><b>macOS</b></summary>

1. Install Xcode from the App Store, accept its license, and run its first-launch setup:
   ```bash
   sudo xcodebuild -runFirstLaunch
   ```
2. Download the Metal toolchain:
   ```bash
   xcodebuild -downloadComponent MetalToolchain
   ```
3. Install [Homebrew](https://brew.sh), then install CMake and MoltenVK:
   ```bash
   brew install cmake molten-vk
   ```

</details>

### 2. Get the source

```bash
git clone https://github.com/nexus382/Hollycast.git
cd Hollycast

# Symlinks must be enabled for this project
git config core.symlinks true

# Fetch the submodules. Run this again whenever submodule versions change.
git submodule update --init --recursive --force
```

`master` is the stable branch. New work lands on `dev` first (`git checkout dev`).

### 3. Build for Windows, Linux or macOS

Run these commands from the `Hollycast` folder:

```bash
# List the presets available on this platform
cmake --list-presets

# Configure. You usually only need to rerun this when files such as CMakeLists.txt change.
cmake --preset <PRESET>

# Build, using the same preset name
cmake --build --preset <PRESET>
```

| Preset | Builds |
| --- | --- |
| `windows`, `linux`, `darwin` | Optimized build |
| `windows-debug`, `linux-debug`, `darwin-debug` | Debug build |
| `windows-unit-tests-debug`, `linux-unit-tests-debug` | Debug build with the unit tests. Running the resulting `Hollycast` executable runs the tests. |

> **Apple Silicon:** the `darwin` preset looks for MoltenVK in `/usr/local/lib`, but Homebrew on Apple Silicon installs it under `/opt/homebrew`. Pass the path when you configure:
> ```bash
> cmake --preset darwin -DMOLTENVK_DYLIB="$(brew --prefix molten-vk)/lib/libMoltenVK.dylib"
> ```

### Native Metal renderer on macOS

Standalone macOS builds include the experimental native Metal backend by default:

```bash
cmake --preset darwin -DMOLTENVK_DYLIB="$(brew --prefix molten-vk)/lib/libMoltenVK.dylib"
cmake --build --preset darwin
```

Then select **Settings > Video > Graphics API > Metal (Experimental)** and choose per-triangle, per-strip or per-pixel sorting. Metal is offered only when its runtime device/pipeline check succeeds; per-pixel support is checked separately. Existing renderer preferences are preserved. `-DUSE_METAL=OFF` remains available for builds that explicitly exclude it. See [the Metal renderer notes](docs/Native%20Metal%20renderer.md) for supported features, limitations and verification.

### Android

**Develop in Android Studio**

1. Install [Android Studio](https://developer.android.com/studio/install).
2. Open the `shell/android-studio` folder in Android Studio. The Hollycast project loads automatically, with the "Android" sidebar on the left and the devices and run configurations at the top right.
3. The `debug` build variant is used by default. For a faster release build, open **View > Tool Windows > Build Variants** and set the active build variant to `developerRelease`.
   - The `release` variant does production code signing and is only meant for store publishing.

**Build from the command line**

```bash
# You must build from shell/android-studio
cd shell/android-studio

# Debug build
./gradlew assembleDebug bundleDebug --parallel
```

If the build hangs or fails, run these commands and then try again:

```bash
# Stop all background Gradle daemons
./gradlew --stop

# Remove all output from previous builds
./gradlew clean
```

## Debugging

### Segmentation faults (SIGSEGV) and access violations

When you debug Hollycast, the debugger will often stop on memory access violations while a game is running. This is normal and expected. The emulator's dynamic recompiler causes these on purpose, and they don't mean there is a memory safety bug. See [Dynarec Architecture](docs/Dynarec%20Architecture.md) for details.

Set your debugger to pass these signals through so they don't interrupt you:

- **LLDB:** use the startup command `process handle -s false -n false -p true SIGSEGV`
- **Android:** if the debugger stops at the `art_sigsegv_fault` breakpoint in the disassembly, delete that breakpoint and resume.
- **VS Code C/C++ on Windows:** use the filter string `!0xC0000005` on the "All Exceptions" breakpoint.

## Support the project

If you enjoy Hollycast, you can support its development on [Ko-fi](https://ko-fi.com/romrevival) or [PayPal](https://paypal.me/AnthonyScordino).

## License and credits

Hollycast is licensed under the [GNU General Public License v2.0](LICENSE).

Hollycast exists because of [flyinghead](https://github.com/flyinghead) and the [Flycast](https://github.com/flyinghead/flycast) contributors, whose work on the core makes this project possible. Thanks as well to the authors of the libraries in [`core/deps`](core/deps) and to everyone who translates Hollycast on Crowdin.
