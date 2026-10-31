#!/usr/bin/env python3
"""Capture a repeatable game frame using Hollycast's TEST_AUTOMATION build.

ROM, BIOS and VMU inputs remain local. Each invocation gets isolated writable data.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

def parse_setting(text):
    key, separator, value = text.partition("=")
    if (not separator or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", key)
            or any(char in value for char in "\r\n\0")):
        raise argparse.ArgumentTypeError("setting must be KEY=VALUE with a valid single-line key and value")
    return key, value


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--app", type=Path, required=True, help="Hollycast executable built with TEST_AUTOMATION=ON")
parser.add_argument("--game", type=Path, required=True)
parser.add_argument("--bios-dir", type=Path, required=True)
parser.add_argument("--vmu", type=Path)
parser.add_argument("--state", type=Path, help="Autoload an isolated savestate before capturing")
parser.add_argument("--renderer", type=int, choices=[0, 4, 5, 7, 8], required=True)
parser.add_argument("--seconds", type=float, default=30,
                    help="Capture time without --input; also sets the replay timeout budget")
parser.add_argument("--input", type=Path, help="Optional replay input file, using emulated SH4 cycle timestamps")
parser.add_argument("--capture-delay-ms", type=int, default=None,
                    help="Emulated milliseconds after replay ends before capture (default: 500 with --state, otherwise 0)")
parser.add_argument("--resolution", type=int, choices=[240, 480, 720, 960, 1440], default=480)
parser.add_argument("--window-size", type=int, nargs=2, metavar=("WIDTH", "HEIGHT"), default=(640, 480),
                    help="SDL window dimensions in points (default: 640 480)")
parser.add_argument("--threaded", action="store_true", help="Enable threaded rendering")
parser.add_argument("--per-strip", action="store_true", help="Use per-strip transparency sorting")
parser.add_argument("--pixel-buffer-mib", type=int, default=512, choices=[512, 1024, 2048, 4096],
                    help="Per-pixel fragment buffer budget")
parser.add_argument("--per-pixel-layers", type=int, default=32, choices=range(1, 257),
                    metavar="1..256", help="Maximum per-pixel transparent layers")
parser.add_argument("--emulate-framebuffer", action="store_true")
parser.add_argument("--foreground", action="store_true", help="Allow a visible window (only with explicit user permission)")
parser.add_argument("--no-modifier-volumes", action="store_true")
parser.add_argument("--setting", action="append", type=parse_setting, default=[], metavar="KEY=VALUE",
                    help="Override a [config] value; repeat for multiple settings")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.capture_delay_ms is not None and not 0 <= args.capture_delay_ms <= 10000:
    parser.error("--capture-delay-ms must be in 0..10000")
capture_delay_ms = args.capture_delay_ms if args.capture_delay_ms is not None else (500 if args.state else 0)
app_bytes = args.app.read_bytes()
if not args.foreground and b"HOLLYCAST_TEST_BACKGROUND" not in app_bytes:
    parser.error("--app does not support hidden TEST_AUTOMATION capture; refusing to launch")
if capture_delay_ms > 0 and b"capture_delay_ms" not in app_bytes:
    parser.error("--app does not support delayed capture; refusing to launch")
if any(value <= 0 or value > 16384 for value in args.window_size):
    parser.error("window dimensions must be in 1..16384")
overrides = dict(args.setting)
effective_config = {
    "pvr.rend": str(args.renderer),
    "bios.UseReios": "yes",
    "Dreamcast.AutoSaveState": "no",
    "Dreamcast.AutoLoadState": "no",
    "rend.Resolution": str(args.resolution),
    "rend.PerStripSorting": "yes" if args.per_strip else "no",
    "rend.ThreadedRendering": "yes" if args.threaded else "no",
    "rend.PixelBufferSize": str(args.pixel_buffer_mib * 1024 * 1024),
    "rend.PerPixelLayers": str(args.per_pixel_layers),
    "rend.EmulateFramebuffer": "yes" if args.emulate_framebuffer else "no",
    "rend.ModifierVolumes": "no" if args.no_modifier_volumes else "yes",
    "FetchBoxart": "no",
}
effective_config.update(overrides)
if args.state:
    if "Dreamcast.AutoLoadState" in overrides and overrides["Dreamcast.AutoLoadState"].lower() not in {"yes", "true", "1"}:
        parser.error("--state requires Dreamcast.AutoLoadState enabled")
    effective_config["Dreamcast.AutoLoadState"] = "yes"
if effective_config["pvr.rend"] not in {"0", "4", "5", "7", "8"}:
    parser.error("pvr.rend override must be one of 0, 4, 5, 7, or 8")
def effective_int(key):
    value = effective_config[key]
    try:
        return int(value, 10)
    except ValueError:
        return value


def effective_bool(key):
    value = effective_config[key]
    if value.lower() in {"yes", "true", "1"}:
        return True
    if value.lower() in {"no", "false", "0"}:
        return False
    return value


runtime = args.output.resolve()
# A fresh directory prevents stale screenshots and mutable saves from passing a rerun.
if runtime.exists() and (not runtime.is_dir() or any(runtime.iterdir())):
    parser.error(f"--output must be a new or empty directory: {runtime}")
runtime.mkdir(parents=True, exist_ok=True)
for filename in ["dc_boot.bin", "dc_flash.bin"]:
    shutil.copyfile(args.bios_dir / filename, runtime / filename)
if args.vmu:
    shutil.copyfile(args.vmu, runtime / "vmu_save_A1.bin")
    if args.vmu.name != "vmu_save_A1.bin":
        shutil.copyfile(args.vmu, runtime / args.vmu.name)
if args.state:
    shutil.copyfile(args.state, runtime / f"{args.game.stem}_10.state")
(runtime / "scripts").mkdir(exist_ok=True)
replay = runtime / "scripts" / (args.game.stem + ".input")
if args.input:
    shutil.copyfile(args.input, replay)
else:
    replay.write_text(f"{int(args.seconds * 200_000_000)} button 0 ffff\n")
(runtime / "emu.cfg").write_text(
    "[config]\n" + "".join(f"{key} = {value}\n" for key, value in effective_config.items())
    + f"[record]\nreplay_input = yes\ncapture_delay_ms = {capture_delay_ms}\n"
    + f"[window]\nwidth = {args.window_size[0]}\nheight = {args.window_size[1]}\nfullscreen = no\n")
environment = dict(os.environ)
# The macOS entry point uses the working directory when HOME is absent.
environment.pop("HOME", None)
environment["MTL_DEBUG_LAYER"] = "1"
if not args.foreground:
    environment["SDL_MAC_BACKGROUND_APP"] = "1"
    environment["HOLLYCAST_TEST_BACKGROUND"] = "1"
else:
    environment.pop("SDL_MAC_BACKGROUND_APP", None)
    environment.pop("HOLLYCAST_TEST_BACKGROUND", None)
(runtime / "capture.json").write_text(json.dumps({
    "game": str(args.game.resolve()), "renderer": int(effective_config["pvr.rend"]),
    "setting_overrides": overrides, "effective_config": effective_config,
    "background": not args.foreground,
    "capture_delay_ms": capture_delay_ms,
    "state_sha256": hashlib.sha256(args.state.read_bytes()).hexdigest() if args.state else None,
    "resolution": effective_int("rend.Resolution"), "window_size": args.window_size,
    "threaded": effective_bool("rend.ThreadedRendering"),
    "per_strip": effective_bool("rend.PerStripSorting"),
    "emulate_framebuffer": effective_bool("rend.EmulateFramebuffer"),
    "pixel_buffer_bytes": effective_int("rend.PixelBufferSize"),
    "pixel_buffer_mib": (effective_int("rend.PixelBufferSize") / (1024 * 1024)
                         if isinstance(effective_int("rend.PixelBufferSize"), int)
                         else effective_int("rend.PixelBufferSize")),
    "per_pixel_layers": effective_int("rend.PerPixelLayers"),
    "modifier_volumes": effective_bool("rend.ModifierVolumes"),
    "app_sha256": hashlib.sha256(app_bytes).hexdigest(),
    "replay_sha256": hashlib.sha256(replay.read_bytes()).hexdigest(),
}, indent=2) + "\n")
with (runtime / "capture.log").open("w") as log:
    result = subprocess.run([str(args.app.resolve()), str(args.game.resolve())],
                            cwd=runtime, env=environment, stdout=log, stderr=subprocess.STDOUT,
                            timeout=max(120, args.seconds * 5))
if result.returncode:
    raise SystemExit(f"Capture failed ({result.returncode}); see {runtime / 'capture.log'}")
log_text = (runtime / "capture.log").read_text(errors="replace")
expected_renderer_log = {
    "4": ": VulkanRenderer::Init",
    "5": ": OITVulkanRenderer::Init",
    "7": "Native Metal regular renderer initialized",
    "8": "Native Metal per-pixel renderer initialized",
}.get(effective_config["pvr.rend"])
if expected_renderer_log and expected_renderer_log not in log_text:
    raise SystemExit(f"Requested renderer was not confirmed initialized. See {runtime / 'capture.log'}")
if args.state:
    expected_state = f"{args.game.stem}_10.state"
    if not any("Loaded state ver" in line and expected_state in line for line in log_text.splitlines()):
        raise SystemExit(f"Requested state was not confirmed loaded. See {runtime / 'capture.log'}")
screenshot = runtime / "screenshot.png"
if not screenshot.is_file():
    raise SystemExit(f"No screenshot; ensure TEST_AUTOMATION=ON. See {runtime / 'capture.log'}")
print(screenshot)
