#!/usr/bin/env python3
# Portions Copyright 2026 The Hollycast Authors
"""Check large OpenBSD resources, exact bytes, pointer ranges, and cache keys.

Usage: python3 tests/regression/cmrc_openbsd.py <temporary-output-dir>
Requires CMake and a Clang that supports the x86_64 OpenBSD target.
"""

import hashlib
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
script = ROOT / "shell/cmake/CMakeRC.cmake"
compiler = ["clang++", "-target", "x86_64-unknown-openbsd7.9", "-O3", "-std=c++17"]


def generate(resource, output, openbsd=True):
    subprocess.run([
        "cmake", "-D_CMRC_GENERATE_MODE=TRUE",
        "-D_CMRC_OPENBSD_CLANG=" + ("TRUE" if openbsd else "FALSE"),
        "-DNAMESPACE=Hollycast", "-DSYMBOL=test",
        "-DINPUT_FILE=" + str(resource), "-DOUTPUT_FILE=" + str(output),
        "-P", str(script),
    ], check=True)


def check_object(path, expected):
    """Inspect the x86_64 ELF payload and its start/end pointer relocations."""
    elf = path.read_bytes()
    assert elf[:6] == b"\x7fELF\x02\x01"  # ELF64, little endian
    shoff = struct.unpack_from("<Q", elf, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize)
                for i in range(shnum)]
    names_section = sections[shstrndx]
    names = elf[names_section[4]:names_section[4] + names_section[5]]
    def name(offset):
        return names[offset:names.index(b"\0", offset)].decode()
    by_name = {name(section[0]): section for section in sections}
    rodata = by_name[".rodata"]
    assert elf[rodata[4]:rodata[4] + rodata[5]] == expected + b"\0"
    relocs = by_name[".rela.data.rel.ro"]
    entries = [struct.unpack_from("<QQq", elf, relocs[4] + i)
               for i in range(0, relocs[5], relocs[9])]
    assert [(offset, info & 0xffffffff, addend) for offset, info, addend in entries] == [
        (0, 1, 0), (8, 1, len(expected))  # R_X86_64_64, with start/end addends
    ]


resource = out / 'resource with "quotes" and \\backslash.bin'
source = out / "embedded.cpp"
obj = out / "embedded.o"
payload = bytes(range(256)) * 4096 + b"\0"  # just over the 1 MiB threshold
resource.write_bytes(payload)
generate(resource, source)
assert ".incbin" in source.read_text()
subprocess.run([*compiler, "-c", str(source), "-o", str(obj)], check=True)
check_object(obj, payload)
preprocessed = subprocess.check_output([*compiler, "-E", str(source)])
assert hashlib.sha256(payload).hexdigest().encode() in preprocessed

# A same-length change must alter the preprocessed input seen by ccache.
payload = payload[:-1] + b"\xff"
resource.write_bytes(payload)
generate(resource, source)
updated = subprocess.check_output([*compiler, "-E", str(source)])
assert updated != preprocessed
assert hashlib.sha256(payload).hexdigest().encode() in updated
subprocess.run([*compiler, "-c", str(source), "-o", str(obj)], check=True)
check_object(obj, payload)

# Empty and threshold-sized files use the original array generator, as do
# non-OpenBSD platforms regardless of size.
for size, openbsd in [(0, True), (1_048_576, True), (0, False), (1_048_577, False)]:
    resource.write_bytes(b"\xa5" * size)
    generate(resource, source, openbsd)
    assert ".incbin" not in source.read_text()
    if size == 0:
        subprocess.run([*compiler, "-c", str(source), "-o", str(obj)], check=True)
        assert b"\0" in obj.read_bytes()

print("OpenBSD resource embedding: exact bytes, pointer ranges, path escaping, cache invalidation and fallback passed")
