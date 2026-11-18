#!/usr/bin/env python3
# Copyright 2026 The Hollycast Authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build deterministic Hollycast catalogue packages using only Python's stdlib."""
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import re
import zipfile


def check(condition, message):
    if not condition:
        raise ValueError(message)


def text(value, name, maximum):
    check(isinstance(value, str) and 0 < len(value.encode()) <= maximum and "\0" not in value,
          f"Invalid {name}")
    return value


def build(root, verify=False):
    root = root.resolve()
    entries, outputs = [], {}
    for folder in sorted((root / "themes").glob("*")):
        check(folder.is_dir() and not folder.is_symlink(), f"Invalid theme folder: {folder}")
        manifest = json.loads((folder / "theme.json").read_text())
        listing = json.loads((folder / "listing.json").read_text())
        theme_id = manifest["id"]
        check(isinstance(theme_id, str) and re.fullmatch(r"[a-z0-9_-]{1,64}", theme_id)
              and folder.name == theme_id, f"Invalid theme id: {theme_id}")
        check(manifest["formatVersion"] == 1, f"Unsupported manifest: {theme_id}")
        version = text(listing["version"], "version", 64)
        check(re.fullmatch(r"[A-Za-z0-9._-]+", version), "Version must be safe in filenames")
        screenshots = listing["screenshots"]
        check(isinstance(screenshots, list) and 1 <= len(screenshots) <= 8,
              f"{theme_id} needs one to eight screenshots")
        for screenshot in screenshots:
            check(isinstance(screenshot, str) and "\\" not in screenshot
                  and not PurePosixPath(screenshot).is_absolute()
                  and not any(c in screenshot for c in "%?#:")
                  and all(p not in ("", ".", "..") for p in screenshot.split("/")),
                  f"Invalid screenshot path: {screenshot}")
            image = root / screenshot
            check(image.resolve().is_relative_to(root) and image.is_file()
                  and not image.is_symlink() and 0 < image.stat().st_size <= 8 * 1024 * 1024,
                  f"Missing or oversized screenshot: {screenshot}")
        files, size = [], 0
        for file in sorted(folder.rglob("*")):
            check(not file.is_symlink(), f"Symlinks are not supported: {file}")
            if file.is_dir() or file.name == "listing.json":
                continue
            relative = file.relative_to(folder).as_posix()
            check(file.is_file() and len(file.relative_to(folder).parts) <= 9
                  and len(relative.encode()) <= 512 and "\\" not in relative and ":" not in relative,
                  f"Invalid package path: {file}")
            size += file.stat().st_size
            files.append(file)
        check(len(files) <= 256 and size <= 64 * 1024 * 1024, f"Package exceeds limits: {theme_id}")
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as package:
            for file in files:
                info = zipfile.ZipInfo(file.relative_to(folder).as_posix(), (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                package.writestr(info, file.read_bytes())
        package_path = f"packages/{theme_id}-{version}.zip"
        check(buffer.tell() <= 64 * 1024 * 1024, f"Theme ZIP exceeds 64 MiB: {theme_id}")
        outputs[package_path] = buffer.getvalue()
        entries.append({"id": theme_id, "name": text(manifest["name"], "name", 160),
                        "creator": text(manifest["author"], "author", 160),
                        "description": text(manifest["description"], "description", 4096),
                        "version": version, "package": package_path, "screenshots": screenshots})
    check(len(entries) <= 512, "Catalogue exceeds 512 themes")
    outputs["catalog.json"] = (json.dumps({"formatVersion": 1, "themes": entries}, indent=2,
                                         ensure_ascii=False) + "\n").encode()
    check(len(outputs["catalog.json"]) <= 2 * 1024 * 1024, "Catalogue exceeds 2 MiB")
    for relative, data in outputs.items():
        path = root / relative
        check(path.resolve().is_relative_to(root) and not path.is_symlink(), f"Unsafe output: {path}")
        if verify:
            check(path.is_file() and path.read_bytes() == data,
                  f"Generated file is stale: {relative}; run python3 build_catalog.py")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    print(f"{'Verified' if verify else 'Built'} {len(entries)} theme(s)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    build(args.root, args.check)
