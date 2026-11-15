# Portions Copyright 2026 The Hollycast Authors
"""Exercise the Unix update transaction with private files and a fake download tool."""
import argparse
import hashlib
import io
import json
import os
import pathlib
import subprocess
import tempfile
import time
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True, type=pathlib.Path)
    parser.add_argument("--work-root", required=True, type=pathlib.Path)
    parser.add_argument("--shell", default="/bin/sh")
    parser.add_argument("--appimage-tool", required=True, type=pathlib.Path)
    parser.add_argument("--case")
    args = parser.parse_args()
    cases = ("success", "nested", "checksum", "unsafe-path", "cancel", "non-appimage", "wrong-architecture", "launch-failure")
    if args.case:
        if args.case not in cases:
            parser.error("Unknown transaction case")
        cases = (args.case,)
    if os.name == "nt":
        parser.error("AppImage transactions require Linux; run this fixture in WSL")
    root = pathlib.Path(tempfile.mkdtemp(prefix="unix-updater-", dir=args.work_root))
    # Use genuine runtime+SquashFS images. An ordinary ELF is a rejection case,
    # not a successful update fixture.
    images = {}
    for name in ("success", "failure"):
        appdir = root / (name + ".AppDir")
        appdir.mkdir()
        (appdir / "fixture.desktop").write_text("[Desktop Entry]\nName=Updater fixture\nExec=AppRun\nIcon=fixture\nType=Application\nCategories=Game;\n")
        (appdir / "fixture.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16"/></svg>')
        app_run = appdir / "AppRun"
        app_run.write_text("#!/bin/sh\nprintf started > new-started\n" +
                           ("exit 23\n" if name == "failure" else "while [ ! -e exit ]; do sleep 0.1; done\n"))
        app_run.chmod(0o755)
        image = root / (name + ".AppImage")
        build = subprocess.run([str(args.appimage_tool.resolve()), "--no-appstream", str(appdir), str(image)],
                               env=dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1"),
                               text=True, capture_output=True, timeout=60)
        assert build.returncode == 0, build.stderr[-2000:]
        images[name] = image.read_bytes()
    results = []
    for case in cases:
        folder = root / case
        install, work, tools = folder / "installed", folder / "request", folder / "tools"
        for directory in (install, work, tools):
            directory.mkdir(parents=True)
        target = install / "Hollycast.AppImage"
        original = b"#!/bin/sh\nprintf restored > old-restarted\n"
        target.write_bytes(original)
        target.chmod(0o755)
        (install / "save.vmu").write_bytes(b"preserve test save")
        payload = images["failure" if case == "launch-failure" else "success"]
        if case == "non-appimage":
            payload = pathlib.Path("/bin/true").read_bytes()
        elif case == "wrong-architecture":
            payload = payload[:18] + b"\xb7\x00" + payload[20:]  # AArch64 e_machine in an otherwise valid image.
        content = io.BytesIO()
        with zipfile.ZipFile(content, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("Hollycast-x86_64.AppImage", payload)
            if case == "unsafe-path":
                archive.writestr("../escape", "forbidden")
        if case == "nested":
            outer = io.BytesIO()
            with zipfile.ZipFile(outer, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("inner.zip", content.getvalue())
            content = outer
        archive = folder / "release.zip"
        archive.write_bytes(content.getvalue())
        curl = tools / "curl"
        curl.write_bytes(b'#!/bin/sh\nwhile [ "$#" -gt 0 ]; do\n if [ "$1" = --output ]; then shift; output=$1; fi\n shift\ndone\ncp "$UPDATER_TEST_ARCHIVE" "$output"\n')
        curl.chmod(0o755)
        if case == "cancel":
            (work / "cancel").touch()
        digest = "0" * 64 if case == "checksum" else hashlib.sha256(archive.read_bytes()).hexdigest()
        url = "https://github.com/nexus382/Hollycast/releases/download/fixture/app.zip"
        env = dict(os.environ, UPDATER_TEST_ARCHIVE=str(archive), APPIMAGE_EXTRACT_AND_RUN="1",
                   PATH=str(tools) + os.pathsep + os.environ["PATH"])
        command = [args.shell, str(args.helper), "linux", url, digest, str(archive.stat().st_size),
                   str(target), "999999999", str(work), str(install)]
        try:
            result = subprocess.run(command, cwd=install, env=env, capture_output=True, text=True, timeout=15)
        finally:
            # Stop the private fixture application even if an assertion fails later.
            (install / "exit").touch()
        status = (work / "status").read_text()
        success = case in ("success", "nested")
        evidence = {"case": case, "exit_code": result.returncode, "status": status,
                    "previous_retained": target.read_bytes() == original, "stderr": result.stderr}
        (folder / "result.json").write_text(json.dumps(evidence, indent=2))
        if success:
            assert result.returncode == 0 and status.startswith("Done"), evidence
            assert target.read_bytes() == payload
            assert (install / "new-started").exists(), "Fixture AppRun never started"
        else:
            assert result.returncode != 0 and status.startswith("Error"), evidence
            assert target.read_bytes() == original, evidence
        if case == "launch-failure":
            deadline = time.monotonic() + 3
            while not (install / "old-restarted").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert (install / "old-restarted").read_text() == "restored", evidence
        assert (install / "save.vmu").read_bytes() == b"preserve test save"
        assert not (work / "package.zip").exists(), "Terminal update retained its download"
        assert not (folder / "escape").exists()
        results.append(evidence)
        print(case + ": passed", flush=True)
    (root / "results.json").write_text(json.dumps(results, indent=2))
    print(str(len(results)) + " Unix transaction cases passed; evidence: " + str(root))


if __name__ == "__main__":
    main()
