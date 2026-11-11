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
    parser.add_argument("--case")
    args = parser.parse_args()
    cases = ("success", "nested", "checksum", "unsafe-path", "cancel", "launch-failure")
    if args.case:
        if args.case not in cases:
            parser.error("Unknown transaction case")
        cases = (args.case,)
    if os.name == "nt" and cases != ("launch-failure",):
        parser.error("Successful ELF execution needs Linux; Windows supports the failed-launch case")
    root = pathlib.Path(tempfile.mkdtemp(prefix="unix-updater-", dir=args.work_root))
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
        payload = b"\x7fELFnot a valid executable" if case == "launch-failure" else pathlib.Path("/bin/true").read_bytes()
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
        env = dict(os.environ, UPDATER_TEST_ARCHIVE=str(archive), PATH=str(tools) + os.pathsep + os.environ["PATH"])
        if os.name == "nt":
            # Git Bash checks the same background-shell failure behavior locally.
            # Linux CI also exercises successful ELF replacement and wrapped packages.
            script = """export PATH="$(cygpath -u "$1"):$PATH"
export UPDATER_TEST_ARCHIVE="$(cygpath -u "$2")"
exec /bin/sh "$(cygpath -u "$3")" linux "$4" "$5" "$6" "$(cygpath -u "$7")" 999999999 "$(cygpath -u "$8")" "$(cygpath -u "$9")"
"""
            command = [args.shell, "--noprofile", "--norc", "-c", script, "unix-updater-test", str(tools), str(archive),
                       str(args.helper), url, digest, str(archive.stat().st_size), str(target), str(work), str(install)]
        else:
            command = [args.shell, str(args.helper), "linux", url, digest, str(archive.stat().st_size),
                       str(target), "999999999", str(work), str(install)]
        result = subprocess.run(command, cwd=install, env=env, capture_output=True, text=True, timeout=15)
        status = (work / "status").read_text()
        success = case in ("success", "nested")
        evidence = {"case": case, "exit_code": result.returncode, "status": status,
                    "previous_retained": target.read_bytes() == original, "stderr": result.stderr}
        (folder / "result.json").write_text(json.dumps(evidence, indent=2))
        if success:
            assert result.returncode == 0 and status.startswith("Done"), evidence
            assert target.read_bytes() == payload
        else:
            assert result.returncode != 0 and status.startswith("Error"), evidence
            assert target.read_bytes() == original, evidence
        if case == "launch-failure":
            deadline = time.monotonic() + 3
            while not (install / "old-restarted").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert (install / "old-restarted").read_text() == "restored", evidence
        assert (install / "save.vmu").read_bytes() == b"preserve test save"
        assert not (folder / "escape").exists()
        results.append(evidence)
        print(case + ": passed", flush=True)
    (root / "results.json").write_text(json.dumps(results, indent=2))
    print(str(len(results)) + " Unix transaction cases passed; evidence: " + str(root))


if __name__ == "__main__":
    main()
