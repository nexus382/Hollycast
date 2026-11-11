#!/bin/sh
# Portions Copyright 2026 The Hollycast Authors
set -eu
export LC_ALL=C
kind=$1
url=$2
expected_hash=$3
expected_size=$4
target=$5
parent_pid=$6
work=$7
previous_cwd=$8
status="$work/status"
installed=false
transaction=
reported=false
publish() {
    printf '%s\n' "$1" > "$work/status.tmp"
    mv -f "$work/status.tmp" "$status"
}
check_cancelled() {
    [ ! -e "$work/cancel" ] || fail 'Update cancelled.'
}
fail() {
    reported=true
    publish "$(printf 'Error\n%s\n' "$1")"
    exit 1
}
on_exit() {
    code=$?
    if [ "$code" -ne 0 ]; then
        if [ -n "$transaction" ] && [ -e "$transaction/previous" ]; then
            if [ "$installed" = true ] && [ -e "$target" ]; then
                mv "$target" "$transaction/failed" || :
            fi
            if [ ! -e "$target" ]; then mv "$transaction/previous" "$target" || :; fi
        fi
        if [ "$reported" = false ]; then
            publish "$(printf 'Error\nThe update could not be installed. The previous application is retained.\n')"
        fi
        if ! kill -0 "$parent_pid" 2>/dev/null && [ -e "$target" ]; then
            if [ "$kind" = macos ]; then /usr/bin/open "$target" || :
            else nohup "$target" >/dev/null 2>&1 &
            fi
        fi
    fi
}
trap on_exit 0
case "$kind" in linux|macos) ;; *) fail 'Unsupported desktop package.' ;; esac
case "$url" in https://github.com/nexus382/Hollycast/releases/download/*/*) ;; *) fail 'Invalid official update URL.' ;; esac
case "$parent_pid:$expected_size" in *[!0-9:]*|:*|*:) fail 'Invalid update request.' ;; esac
[ "${#expected_hash}" = 64 ] || fail 'Invalid update checksum.'
case "$expected_hash" in *[!a-f0-9]*) fail 'Invalid update checksum.' ;; esac
for tool in curl unzip mktemp; do
    command -v "$tool" >/dev/null 2>&1 || fail "The updater requires $tool on this installation."
done
[ -e "$target" ] || fail 'The installed application no longer exists.'
transaction=$(mktemp -d "$(dirname "$target")/.hollycast-update.XXXXXX") || fail 'The installation folder is not writable. Move Hollycast to a writable location and retry.'
check_cancelled
publish Downloading
archive="$work/package.zip"
curl --fail --location --proto '=https' --proto-redir '=https' --connect-timeout 15 --max-time 300 \
    --max-filesize "$expected_size" --output "$archive" "$url" || fail 'The update download failed. Please retry.'
check_cancelled
actual_size=$(wc -c < "$archive" | tr -d '[:space:]')
[ "$actual_size" = "$expected_size" ] || fail 'The downloaded package has the wrong size.'
if [ "$kind" = macos ]; then
    actual_hash=$(/usr/bin/shasum -a 256 < "$archive") || fail 'Cannot verify the downloaded package.'
else
    command -v sha256sum >/dev/null 2>&1 || fail 'The updater requires sha256sum.'
    actual_hash=$(sha256sum < "$archive") || fail 'Cannot verify the downloaded package.'
fi
actual_hash=${actual_hash%% *}
[ "$actual_hash" = "$expected_hash" ] || fail 'The downloaded package failed SHA-256 verification.'
prepared=
depth=0
while [ "$depth" -lt 3 ]; do
    check_cancelled
    # Bound extraction before creating files, including CI ZIPs wrapped inside ZIPs.
    unpacked=$(unzip -Z -t "$archive" | awk '{for (i=1; i<NF-1; i++) if ($(i+1)=="bytes" && $(i+2)=="uncompressed,") print $i}')
    case "$unpacked" in ''|*[!0-9]*) fail 'Cannot read the update archive size.' ;; esac
    [ "$unpacked" -le 536870912 ] || fail 'The unpacked update is too large.'
    list="$work/entries-$depth"
    unzip -Z1 "$archive" > "$list" || fail 'The update is not a valid ZIP archive.'
    [ "$(wc -l < "$list")" -le 10000 ] || fail 'The update archive has too many files.'
    # Validate every name before extraction; never evaluate a name as shell code or a pattern.
    while IFS= read -r entry; do
        case "$entry" in /*|../*|*/../*|*/..|*\\*|*:*|*\**|*\?*|*\[*) fail 'The update archive contains an unsafe path.' ;; esac
    done < "$list"
    if [ "$kind" = linux ]; then
        entry=$(grep -E '(^|/)[^/]+\.AppImage$' "$list" || :)
        if [ -n "$entry" ]; then
            [ "$(printf '%s\n' "$entry" | wc -l)" -eq 1 ] || fail 'The update contains multiple AppImages.'
            [ "$(uname -m)" = x86_64 ] || fail 'No matching AppImage is published for this architecture.'
            prepared="$transaction/new.AppImage"
            unzip -p "$archive" "$entry" > "$prepared" || fail 'Cannot extract the AppImage.'
            [ "$(wc -c < "$prepared")" -le 536870912 ] || fail 'The unpacked application is too large.'
            magic=$(dd if="$prepared" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d '[:space:]')
            [ "$magic" = 7f454c46 ] || fail 'The update is not an executable AppImage.'
            chmod 755 "$prepared"
            cp -p "$target" "$transaction/previous" || fail 'Cannot preserve the current application for recovery.'
            break
        fi
    else
        app=$(sed -n 's#^\(.*\.app\)/Contents/Info.plist$#\1#p' "$list")
        if [ -n "$app" ]; then
            [ "$(printf '%s\n' "$app" | wc -l)" -eq 1 ] || fail 'The update contains multiple applications.'
            # CI packages dereference bundle links. Reject archive links rather than allowing
            # an extraction to redirect writes outside the private transaction directory.
            if unzip -Z -l "$archive" | grep -q '^l'; then fail 'The update archive contains unsupported symbolic links.'; fi
            /usr/bin/ditto -x -k "$archive" "$transaction/payload" || fail 'Cannot extract the application.'
            prepared="$transaction/payload/$app"
            /usr/bin/codesign --verify --deep --strict --all-architectures "$prepared" || fail 'The application signature check failed.'
            installed_id=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$target/Contents/Info.plist")
            new_id=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$prepared/Contents/Info.plist")
            [ "$installed_id" = "$new_id" ] || fail 'The update is for a different application.'
            break
        fi
    fi
    nested=$(grep -E '\.zip$' "$list" || :)
    [ -n "$nested" ] && [ "$(printf '%s\n' "$nested" | wc -l)" -eq 1 ] || fail 'The update archive does not contain the matching application.'
    next="$work/inner-$depth.zip"
    unzip -p "$archive" "$nested" > "$next" || fail 'Cannot extract the wrapped package.'
    [ "$(wc -c < "$next")" -le 536870912 ] || fail 'The wrapped package is too large.'
    archive=$next
    depth=$((depth + 1))
done
[ -n "$prepared" ] && [ -e "$prepared" ] || fail 'The nested package format is unsupported.'
check_cancelled
publish Ready
waited=0
while kill -0 "$parent_pid" 2>/dev/null; do
    check_cancelled
    [ "$waited" -lt 180 ] || fail 'Hollycast did not exit; the update was not applied.'
    sleep 1
    waited=$((waited + 1))
done
check_cancelled
if [ "$kind" = macos ]; then
    mv "$target" "$transaction/previous" || fail 'Cannot preserve the installed application.'
fi
# Linux rename replaces its file atomically. macOS retains the old bundle and
# restores it from the exit trap if publishing the prepared bundle fails.
mv -f "$prepared" "$target" || fail 'Cannot install the verified update.'
installed=true
publish Done
cd "$previous_cwd" 2>/dev/null || cd "$(dirname "$target")"
if [ "$kind" = macos ]; then /usr/bin/open "$target"
else nohup "$target" >/dev/null 2>&1 &
fi
