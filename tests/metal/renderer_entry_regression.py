#!/usr/bin/env python3
"""Link an isolated renderer fixture against an already-built Ninja app.

Configure with USE_METAL=ON, TEST_AUTOMATION=ON and
CMAKE_EXPORT_COMPILE_COMMANDS=ON, then build Hollycast before running this.
All replacement objects and the test executable live in a temporary directory.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="configured and fully built Ninja directory")
    args = parser.parse_args()
    build = args.build.resolve()
    commands = json.loads((build / "compile_commands.json").read_text())

    def command_for(name):
        return next(c for c in commands if c["file"].endswith("/" + name))

    renderer = command_for("metal_renderer.mm")
    if "-DTEST_AUTOMATION" not in renderer["command"]:
        raise RuntimeError("build must enable TEST_AUTOMATION for hidden windows")
    ninja = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "Hollycast"], text=True)
    link = next(line for line in reversed(ninja.splitlines())
                if "/usr/bin/c++" in line and " -o Hollycast.app/" in line)
    link_args = shlex.split(link.split(" && ", 1)[1].split(" && ", 1)[0])

    with tempfile.TemporaryDirectory(prefix="hollycast-metal-entry-") as directory:
        directory = Path(directory)
        def compile_object(name, output, extra, source=None):
            entry = command_for(name)
            cmd = shlex.split(entry["command"])
            cmd[cmd.index("-o") + 1] = str(output)
            # Keep compiler dependency side effects out of the shared app build.
            if "-MF" in cmd:
                cmd[cmd.index("-MF") + 1] = str(output.with_suffix(".d"))
            if "-MT" in cmd:
                cmd[cmd.index("-MT") + 1] = str(output)
            if source:
                cmd[cmd.index(entry["file"])] = str(source)
            subprocess.run(cmd + extra, cwd=entry["directory"], check=True)

        fixture = directory / "fixture.o"
        ta = directory / "ta.o"
        overlay = directory / "overlay.o"
        compile_object("metal_renderer.mm", fixture, ["-fno-access-control"],
                       ROOT / "tests/metal/renderer_entry_regression.mm")
        compile_object("ta_vtx.cpp", ta, ["-Dta_parse=hollycast_original_ta_parse"])
        # Keep unrelated overlay entry points real while intercepting timing calls.
        compile_object("performance_overlay.cpp", overlay, [
            "-DrecordGpuTimingSample=fixture_original_recordGpuTimingSample",
            "-DsetGpuTimingSupported=fixture_original_setGpuTimingSupported",
            "-DisGpuTimingRequested=fixture_original_isGpuTimingRequested",
            "-DgetGpuTimingEpoch=fixture_original_getGpuTimingEpoch",
        ])
        excluded = ("/performance_overlay.cpp.o", "/metal_renderer.mm.o", "/metal_imgui.mm.o", "/ta_vtx.cpp.o", "/SDLApplicationDelegate.mm.o")
        link_args = [arg for arg in link_args if not any(part in arg for part in excluded)]
        executable = directory / "renderer_entry_regression"
        link_args[link_args.index("-o") + 1] = str(executable)
        subprocess.run(link_args + [str(fixture), str(ta), str(overlay)], cwd=build, check=True)
        env = os.environ.copy()
        env.pop("HOME", None)
        env.update(MTL_DEBUG_LAYER="1", SDL_MAC_BACKGROUND_APP="1", HOLLYCAST_TEST_BACKGROUND="1")
        subprocess.run([str(executable)], cwd=directory, env=env, check=True)


if __name__ == "__main__":
    main()
