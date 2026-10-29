#!/usr/bin/env bash
set -euo pipefail

mode="${1:-run}"
case "$mode" in
  run|--build-only|--debug|--logs|--telemetry|--verify) ;;
  *) echo "usage: $0 [--build-only|--debug|--logs|--telemetry|--verify]" >&2; exit 2 ;;
esac

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$project_root/build-launchbox-app"
app_bundle="$build_dir/Hollycast.app"
app_binary="$app_bundle/Contents/MacOS/Hollycast"

if [[ "$mode" != --build-only ]]; then
  pkill -TERM -x Hollycast >/dev/null 2>&1 || true
fi

cmake -S "$project_root" -B "$build_dir" -G Ninja \
  -DCMAKE_PROJECT_INCLUDE="$project_root/script/macos-build.cmake" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
  -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF \
  -DUSE_VULKAN=OFF -DUSE_BREAKPAD=OFF -DUSE_DISCORD=OFF \
  -DUSE_OPENMP=OFF -DUSE_LUA=OFF
cmake --build "$build_dir" --target Hollycast --parallel 8

case "$mode" in
  --build-only) exit 0 ;;
  --debug) exec lldb -- "$app_binary" ;;
  *) /usr/bin/open -n "$app_bundle" ;;
esac

case "$mode" in
  --logs|--telemetry)
    exec /usr/bin/log stream --info --style compact --predicate 'process == "Hollycast"'
    ;;
  --verify)
    for attempt in 1 2 3 4 5; do
      if pgrep -x Hollycast >/dev/null; then
        echo "Hollycast is running: $app_bundle"
        exit 0
      fi
      sleep 1
    done
    echo "Hollycast did not remain running" >&2
    exit 1
    ;;
esac
