# 009. Enforce theme and artwork byte limits during HTTP transfer

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 9 | P1 | L | Medium | Network resource limits | `ebf8fbb79` · 2026-11-26 |

The existing theme and image limits are checked after the response has been collected. The client can allocate the entire body before the policy takes effect. Apply the selected limit to delivered bytes across native transports while keeping unbounded callers and larger catalogue downloads compatible.

## Before starting

Work from the Hollycast repository root. The native Google Test commands below use Linux, the pinned submodules and the prerequisites in [the build guide](../README.md). Run the Baseline gate before editing. Branch from `dev` as `fix/bounded-asset-downloads`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'core/oslib/http_client.h' \
  'core/oslib/http_client.cpp' \
  'shell/apple/common/http_client.mm' \
  'shell/uwp/http_client.cpp' \
  'shell/android-studio/flycast/src/main/jni/src/http_client.h' \
  'shell/android-studio/flycast/src/main/java/com/hollycast/emulator/emu/HttpClient.java' \
  'core/ui/theme_repository.cpp' \
  'core/ui/theme_repository.h' \
  'core/ui/boxart/scraper.cpp' \
  'core/ui/boxart/scraper.h' \
  'core/ui/boxart/screenscraper.cpp' \
  'core/ui/boxart/screenscraper.h' \
  'core/ui/boxart/gamesdb.cpp' \
  'core/ui/boxart/gamesdb.h' \
  'core/ui/boxart/launchbox.cpp' \
  'tests/src/network/HttpTimeoutTest.cpp' \
  'tests/src/ui/ThemeRepositoryTest.cpp' \
  'tests/src/ui/TheGamesDbTest.cpp' \
  'tests/src/ui/ScreenScraperTest.cpp' \
  'tests/src/ui/LaunchBoxTest.cpp'
```

## Current behavior and local conventions

Theme download checks size after its default or injected transport has returned. [core/ui/theme_repository.cpp:69](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/theme_repository.cpp#L69)

```cpp
		http::init();
	const int status = transport ? transport(url, bytes) : http::get(url, bytes);
	require(http::success(status), "Theme download failed (HTTP " + std::to_string(status) + "). Check your connection and retry.");
	require(!bytes.empty() && bytes.size() <= limit, "Theme download is empty or exceeds the size limit.");
```

Shared provider media currently enforces the 64 MiB limit after the GET. [core/ui/boxart/scraper.cpp:368](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/ui/boxart/scraper.cpp#L368)

```cpp
    const int status = transport ? transport(url, bytes, type) : http::get(url, bytes, type);
    if (status == 404) return false;
    int width = 0, height = 0, channels = 0;
    if (!http::success(status) || bytes.empty() || bytes.size() > 64 * 1024 * 1024
            || type.substr(0, 6) != "image/" || !stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels)
            || width <= 0 || height <= 0 || static_cast<u64>(width) * height > 32 * 1024 * 1024)
```

The curl callback appends each block to an unrestricted vector. [core/oslib/http_client.cpp:444](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/core/oslib/http_client.cpp#L444)

```cpp
static size_t receiveData(void *buffer, size_t size, size_t nmemb, std::vector<u8> *recvBuffer)
{
	recvBuffer->insert(recvBuffer->end(), (u8 *)buffer, (u8 *)buffer + size * nmemb);
	return nmemb * size;
```

The existing HTTP tests use Asio on a private ephemeral loopback port. [tests/src/network/HttpTimeoutTest.cpp:12](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/src/network/HttpTimeoutTest.cpp#L12)

```cpp
class StalledServer
{
	asio::io_context io;
	asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
	asio::ip::tcp::socket socket{io};
```

## Scope

- `core/oslib/http_client.h`
- `core/oslib/http_client.cpp`
- `shell/apple/common/http_client.mm`
- `shell/uwp/http_client.cpp`
- `shell/android-studio/flycast/src/main/jni/src/http_client.h`
- `shell/android-studio/flycast/src/main/java/com/hollycast/emulator/emu/HttpClient.java`
- `core/ui/theme_repository.cpp`
- `core/ui/theme_repository.h`
- `core/ui/boxart/scraper.cpp`
- `core/ui/boxart/scraper.h`
- `core/ui/boxart/screenscraper.cpp`
- `core/ui/boxart/screenscraper.h`
- `core/ui/boxart/gamesdb.cpp`
- `core/ui/boxart/gamesdb.h`
- `core/ui/boxart/launchbox.cpp`
- `tests/src/network/HttpTimeoutTest.cpp`
- `tests/src/ui/ThemeRepositoryTest.cpp`
- `tests/src/ui/TheGamesDbTest.cpp`
- `tests/src/ui/ScreenScraperTest.cpp`
- `tests/src/ui/LaunchBoxTest.cpp`

Scope is GET response bodies for existing bounded consumers. Keep request/response headers, MIME types, cancellation, deadlines, current provider backoff, injected test transports and POST behavior compatible. Large LaunchBox Metadata.zip must retain its own policy.

## Implementation and verification

### 1. Add an opt-in limit to GET

Expose the byte budget through the headers and content-type GET forms while preserving existing source-compatible calls. Specify a distinct unlimited default and an unambiguous failure result with no partially successful body. Check a known Content-Length early when meaningful, then check each block against the remaining budget using overflow-safe arithmetic. Test with tiny synthetic bodies on loopback.

Run the **Baseline** command below. Linux configures and builds. The named existing suites run and pass.

### 2. Implement the budget in every production transport

Handle both WinINet paths and curl in the shared source. Apple needs a streaming delegate for bounded requests instead of a completion that first materializes NSData. UWP needs headers-first completion and bounded stream reads in place of whole-buffer reads. Pass the budget through Android JNI and check before each write into ByteArrayOutputStream. Cancel/close and discard partial data on overflow. Keep timeout and cancellation resources alive until callbacks are finished.

Run the **Build** command below. The unit-test executable links successfully.

### 3. Pass real consumer limits through to transport

Use 2 MiB for theme catalogue, 8 MiB for preview and 64 MiB for package. Keep the shared media cap at 64 MiB and ScreenScraper’s separate 32 MiB image and 4 MiB JSON limits. Trace provider callbacks that install a default lambda, so ordinary requests actually reach bounded GET. Preserve the existing image dimension/decoded pixel and archive expansion checks. Compile each native implementation.

Run the **Windows build**, **Apple build**, **Android build** and **UWP build** commands below on their named hosts. The Linux **Build** gate covers curl. Record each result individually.

### 4. Verify exact boundary and consumer behavior

Use a very small opt-in budget and fixed local data. Cover exact limit, one extra byte, a chunked response, no Content-Length, an early cancellation and an unlimited caller. An over-limit fetch must leave no published theme or image. Record target-specific runtime results with the associated build.

Run the **Focused** command below. The new StopsAtLimitWithoutContentLength case is registered. The selected tests run and pass.

## Test plan

- Use kilobytes or less and an explicit small budget. Check the maximum accepted bytes and the empty failure body, plus cancellation and timeout status. Reuse the existing stalled-server fixture style.
- For each provider test the default HTTP path or a limit-aware seam as well as injected transport returns. An accepted exact-size payload must proceed to the existing format checks. Large catalogue code keeps its explicit call policy.
- Verify both desktop Windows paths, native Apple, UWP, Android and curl builds. Record native execution where available and distinguish it from successful compilation.

## Verification commands

**Baseline**

```sh
cmake --preset linux-unit-tests-debug && cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_filter='HttpTimeoutTest.*:ThemeRepositoryTest.*'
```

Linux configures and builds. The named existing suites run and pass.

**Build**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4
```

The unit-test executable links successfully.

**Focused**

```sh
cmake --build --preset linux-unit-tests-debug --parallel 4 && ./build-unit-tests-debug/Hollycast --gtest_list_tests --gtest_filter='HttpBodyLimitTest.*:HttpTimeoutTest.*:ThemeRepositoryTest.*:TheGamesDbTest.*:ScreenScraperTest.*:LaunchBoxMediaTest.*' | rg -q '^[[:space:]]+StopsAtLimitWithoutContentLength([[:space:]]|$)' && ./build-unit-tests-debug/Hollycast --gtest_filter='HttpBodyLimitTest.*:HttpTimeoutTest.*:ThemeRepositoryTest.*:TheGamesDbTest.*:ScreenScraperTest.*:LaunchBoxMediaTest.*'
```

The new StopsAtLimitWithoutContentLength case is registered. The selected tests run and pass.

**Windows build**

```sh
cmake --preset windows-unit-tests-debug && cmake --build --preset windows-unit-tests-debug
```

Windows host with the repository prerequisites builds WinINet.

**Apple build**

```sh
cmake -S . -B build-http-apple -G Xcode -DUSE_VULKAN=OFF && cmake --build build-http-apple --config Debug --target Hollycast
```

macOS builds the native HTTP implementation.

**Android build**

```sh
(cd shell/android-studio && ./gradlew :flycast:assembleDebug :flycast:testDebugUnitTest)
```

An Android SDK/NDK host compiles Java and JNI and passes host tests.

**UWP build**

```sh
cmake -S . -B build-http-uwp -G 'Visual Studio 17 2022' -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.22000.0 && cmake --build build-http-uwp --config Release
```

A Windows UWP SDK host builds the streaming request path.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] The named test exercises streaming without a declared length. All bounded asset calls enforce their configured budget before whole-body buffering. Each changed platform implementation has build evidence.
- [x] The step gates and applicable platform checks pass, with the host and observed results recorded.
- [x] The **Scope** gate matches the listed files. Record completion in the [plan index](README.md), or send the evidence to the reviewer maintaining it.

## Independent review

Reviewed `55a75b6c`. Five body-limit tests, two timeout tests and the asset consumer suites passed on macOS and Ubuntu arm64. All seven HTTP tests also passed in a MinGW x64 executable using WinINet under Wine. The Android Java client passed 12 loopback cases with its pinned Apache libraries on a host JVM. The UWP implementation retains the original header timeout and unlimited default body time, and releases streams through C++/CX scope. The arm64 Android app and instrumentation APKs built with SDK 36.1 and NDK 29.0.14206865. The Windows Store SDK built and packaged the application for Win32, x64 and ARM64. [UWP SDK build](https://github.com/nexus382/Hollycast/actions/runs/45910521617)

## Stop conditions

If a platform API prebuffers an entire response before handing out a stream, use its headers-first or delegate mode before claiming bounded behavior. If a target SDK is unavailable, identify its outstanding build separately. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

A new GET wrapper should retain a caller-supplied limit through all forwarding layers. Compare budget policies separately for image bytes, decoded pixels, theme expansion and bulk metadata.
