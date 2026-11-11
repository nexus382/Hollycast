# Portions Copyright 2026 The Hollycast Authors
"""Runs real helper replacements only against disposable dummy installations."""
import argparse
import ctypes
import hashlib
import io
import json
import pathlib
import shutil
import subprocess
import tempfile
import time
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wait_for(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for helper")


def main():
    parser = argparse.ArgumentParser()
    for name in ("helper", "parent", "payload"):
        parser.add_argument("--" + name, required=True, type=pathlib.Path)
    parser.add_argument("--case", help="Run one transaction case when reproducing a failure")
    args = parser.parse_args()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CreateFileW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                  ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
    kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
    # Retain fixtures and logs for diagnosis. No path in the real installation is used.
    # The helper may live in artifact/bin, which CI publishes recursively. Keep all
    # retained dummy apps and packages beside the test executables, never beside it.
    root = pathlib.Path(tempfile.mkdtemp(prefix="hollycast-updater-", dir=args.parent.parent))
    cases = ("success", "nested", "bad-checksum", "bad-executable", "cancel-before", "cancel-ready", "rollback", "parent-timeout")
    if args.case:
        if args.case not in cases:
            parser.error("Unknown transaction case")
        cases = (args.case,)
    results = []
    for case in cases:
        folder = root / case
        install, work = folder / "installed", folder / "request"
        install.mkdir(parents=True)
        work.mkdir()
        target = install / "Hollycast.exe"
        shutil.copy2(args.parent, target)
        shutil.copy2(args.helper, install / "HollycastUpdater.exe")
        shutil.copy2(args.helper, work / "HollycastUpdater.exe")
        (install / "emu.cfg").write_text("keep my settings")
        (install / "save.vmu").write_bytes(b"keep my saves")
        original = sha(target)
        old_helper = sha(install / "HollycastUpdater.exe")
        content = io.BytesIO()
        with zipfile.ZipFile(content, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("Hollycast.exe", b"bad executable" if case == "bad-executable" else args.payload.read_bytes())
            archive.writestr("HollycastUpdater.exe", args.helper.read_bytes())
            archive.writestr("emu.cfg", "overwrite forbidden")
            archive.writestr("../escape", "must never extract")
        if case == "nested":
            wrapper = io.BytesIO()
            with zipfile.ZipFile(wrapper, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("Hollycast.zip", content.getvalue())
            content = wrapper
        package = work / "package.zip"
        package.write_bytes(content.getvalue())
        parent = subprocess.Popen([str(target)], cwd=install)
        helper = None
        lock = None
        try:
            wait_for(lambda: (install / "old-started").exists())
            manifest = {"url": "https://github.com/nexus382/Hollycast/releases/download/fixture/test.zip",
                        "sha256": "0" * 64 if case == "bad-checksum" else sha(package),
                        "size": package.stat().st_size, "target": str(target), "pid": parent.pid, "cwd": str(install)}
            (work / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            if case == "cancel-before":
                (work / "cancel").touch()
            if case == "rollback":
                lock = kernel.CreateFileW(str(install / "HollycastUpdater.exe"), 0x80000000, 1, None, 3, 0, None)
                assert lock not in (None, ctypes.c_void_p(-1).value), "Cannot set test file lock"
            helper = subprocess.Popen([str(work / "HollycastUpdater.exe"), "--manifest", str(work / "manifest.json"), "--apply"], cwd=work)
            def status():
                try:
                    return (work / "status").read_text()
                except (FileNotFoundError, PermissionError):
                    return ""
            wait_for(lambda: status().startswith(("Ready", "Error")))
            if case in ("success", "nested", "rollback"):
                assert status().startswith("Ready"), status()
                (install / "exit").touch()
                parent.wait(timeout=10)
            elif case == "cancel-ready":
                assert status().startswith("Ready"), status()
                (work / "cancel").touch()
            helper.wait(timeout=195 if case == "parent-timeout" else 20)
            if case in ("success", "nested"):
                assert helper.returncode == 0 and status().startswith("Done"), status()
                wait_for(lambda: (install / "new-started").exists())
                assert sha(target) == sha(args.payload)
            else:
                assert helper.returncode != 0 and status().startswith("Error"), status()
                assert sha(target) == original, "Failed update changed the application"
                assert sha(install / "HollycastUpdater.exe") == old_helper
                if case == "rollback":
                    wait_for(lambda: len((install / "old-started").read_text()) >= 2)
                else:
                    assert parent.poll() is None, "Failure closed the original app"
            assert (install / "emu.cfg").read_text() == "keep my settings"
            assert (install / "save.vmu").read_bytes() == b"keep my saves"
            assert not (folder / "escape").exists() and not (work / "escape").exists()
            results.append({"case": case, "status": "passed"})
            print(case + ": passed", flush=True)
        finally:
            if lock not in (None, ctypes.c_void_p(-1).value):
                kernel.CloseHandle(lock)
            (install / "exit").touch()
            if parent.poll() is None:
                parent.wait(timeout=10)
            if helper is not None and helper.poll() is None:
                helper.terminate()
                helper.wait(timeout=10)
    (root / "results.json").write_text(json.dumps(results, indent=2))
    print(str(len(results)) + " disposable Windows installation cases passed; evidence: " + str(root))


if __name__ == "__main__":
    main()
