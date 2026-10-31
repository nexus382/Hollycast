#!/usr/bin/env python3
"""Run isolated Metal option smoke captures from a local game savestate.

This checks startup, backend selection, state loading, GPU validation and visible
output. Shader, presentation and reference-image tests supply semantic coverage;
these captures alone do not establish pixel parity for each option.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import re
import subprocess
import sys

from PIL import Image, ImageChops


def cases():
    yield "sorting-triangle", 7, {"rend.PerStripSorting": "no"}
    yield "sorting-strip", 7, {"rend.PerStripSorting": "yes"}
    yield "sorting-pixel", 8, {}
    ranges = {
        "resolution": ("rend.Resolution", [240, 360, 480, 600, 720, 840, 960, 1080, 1200, 1320,
                                            1440, 1680, 1920, 2160, 2400, 2640, 2880, 3360, 3840, 4320]),
        "anisotropy": ("rend.AnisotropicFiltering", [1, 2, 4, 8, 16]),
        "texture-filter": ("rend.TextureFiltering", [0, 1, 2]),
        "reconstruction": ("rend.SampleTextureFilter", list(range(14)) + list(range(15, 20))),
        "filter-size": ("rend.MaxFilteredTextureSize", [8, 16, 32, 64, 128, 256, 512, 1024]),
        "screen-stretch": ("rend.ScreenStretching", [100, 125, 150, 175, 200, 225, 250]),
        "auto-skip": ("pvr.AutoSkipFrame", [0, 1, 2]),
        "frame-skip": ("ta.skip", list(range(7))),
        "threads": ("pvr.MaxThreads", [1, 2, 3, 4, 8]),
        "texture-upscale": ("rend.TextureUpscale2", [1, 2, 3, 4, 5, 6]),
        "depth-scale": ("rend.ExtraDepthScale", [.1, 1, 10]),
        "layers": ("rend.PerPixelLayers", [8, 16, 32, 64, 128]),
        "buffer-mib": ("rend.PixelBufferSize", [n * 1024 * 1024 for n in [512, 1024, 2048, 4096]]),
        "preload": ("rend.PreloadCustomTextures", [0, 1, 2]),
        "routing-height": ("rend.VideoRoutingVRes", [120, 480, 1080, 2160]),
    }
    for group, (key, values) in ranges.items():
        for value in values:
            settings = {key: str(value)}
            if group == "filter-size": settings["rend.SampleTextureFilter"] = "12"
            if group == "preload": settings["rend.CustomTextures"] = "yes"
            if group == "routing-height": settings.update({"rend.VideoRouting": "yes", "rend.VideoRoutingScale": "yes"})
            yield f"{group}-{value}", 8, settings
    booleans = {
        "fog": "rend.Fog", "shadows": "rend.ModifierVolumes",
        "native-depth": "rend.NativeDepthInterpolation", "rtt-vram": "rend.RenderToTextureBuffer",
        "framebuffer": "rend.EmulateFramebuffer", "delay-swap": "rend.DelayFrameSwapping",
        "bleeding-edge": "rend.FixUpscaleBleedingEdge", "mipmaps": "rend.UseMipmaps",
        "translucent-depth-mask": "rend.TranslucentPolygonDepthMask", "threaded": "rend.ThreadedRendering",
        "integer-scale": "rend.IntegerScale", "interpolation": "rend.LinearInterpolation",
        "vsync": "rend.vsync", "widescreen": "rend.WideScreen",
        "super-widescreen": "rend.SuperWideScreen", "widescreen-hacks": "rend.WidescreenGameHacks",
        "stretch-fill": "rend.StretchToFill", "rotation": "rend.Rotate90",
        "custom-textures": "rend.CustomTextures", "dump-textures": "rend.DumpTextures",
        "unique-dumps": "rend.DumpUniqueTextures", "replacement-dumps": "rend.DumpReplacedTextures",
        "vmus": "rend.FloatVMUs", "routing": "rend.VideoRouting", "routing-scale": "rend.VideoRoutingScale",
    }
    for group, key in booleans.items():
        for value in ["no", "yes"]:
            settings = {key: value}
            if group == "super-widescreen": settings["rend.WideScreen"] = "yes"
            if group == "routing-scale": settings["rend.VideoRouting"] = "yes"
            if group in {"unique-dumps", "replacement-dumps"}: settings["rend.DumpTextures"] = "yes"
            yield f"{group}-{value}", 8, settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ["app", "game", "bios-dir", "state", "output"]:
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--vmu", type=Path)
    parser.add_argument("--select", action="append", default=[], help="Only names starting with this prefix; repeatable")
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("--output must be new or empty")
    args.output.mkdir(parents=True, exist_ok=True)
    selected = [c for c in cases() if not args.select or any(c[0].startswith(prefix) for prefix in args.select)]
    if not selected: parser.error("no cases selected")
    script = Path(__file__).with_name("capture_game.py")
    def run(case):
        name, renderer, settings = case
        output = args.output / name
        command = [sys.executable, str(script), "--app", str(args.app.resolve()), "--game", str(args.game.resolve()),
                   "--bios-dir", str(args.bios_dir.resolve()), "--state", str(args.state.resolve()),
                   "--renderer", str(renderer), "--seconds", "1", "--output", str(output.resolve())]
        if args.vmu: command += ["--vmu", str(args.vmu.resolve())]
        for key, value in settings.items(): command += ["--setting", f"{key}={value}"]
        result = subprocess.run(command, capture_output=True, text=True)
        report = {"name": name, "renderer": renderer, "settings": settings, "status": "failed"}
        if result.returncode:
            report["error"] = (result.stderr or result.stdout).strip()[-2000:]
            return report
        log = (output / "capture.log").read_text(errors="replace")
        if re.search(r"Metal .*failed|Validation Error|Execution of the command buffer was aborted", log):
            report["error"] = "GPU error in capture.log"
            return report
        with Image.open(output / "screenshot.png") as image:
            image = image.convert("RGB")
            report["dimensions"] = image.size
            red, green, blue = image.split()
            visible = sum(ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()[41:])
            report["visible_pixels"] = visible
            if visible == 0:
                report["error"] = "all-black output"
                return report
            if name.startswith("resolution-"):
                height = int(settings["rend.Resolution"])
                expected = (int(round((height * 4 / 3) / 2)) * 2, height)
                if image.size != expected:
                    report["error"] = f"expected {expected}, got {image.size}"
                    return report
        report["status"] = "passed-runtime"
        return report
    # Large render targets and texture upscaling run alone to bound memory pressure.
    heavy = [c for c in selected if int(c[2].get("rend.Resolution", 480)) >= 2160
             or int(c[2].get("rend.TextureUpscale2", 1)) >= 4]
    light = [c for c in selected if c not in heavy]
    reports = []
    def record(report):
        reports.append(report)
        (args.output / "results.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"{report['status']}: {report['name']}" + (f" — {report['error']}" if "error" in report else ""), flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(run, c) for c in light]): record(future.result())
    for case in heavy: record(run(case))
    failures = sum(r["status"] != "passed-runtime" for r in reports)
    print(f"{len(reports)-failures}/{len(reports)} runtime option captures passed", flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
