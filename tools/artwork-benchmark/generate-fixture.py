from pathlib import Path
import argparse
import json
import struct


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


parser = argparse.ArgumentParser(description="Generate sparse Dreamcast CUE tracks without embedded artwork.")
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[2] / "build-artwork-benchmark/library")
parser.add_argument("--count", type=positive_int, default=1000)
parser.add_argument("--track-mib", type=positive_int, default=700)
args = parser.parse_args()
root = args.output.resolve()
root.parent.mkdir(parents=True, exist_ok=True)
root.mkdir(exist_ok=False)
for index in range(args.count):
    stem = f"game-{index:04d}"
    header = bytearray(b" " * 256 + b"\0" * (2048 - 256))
    header[0:16] = b"SEGA SEGAKATANA "
    header[16:32] = b"SEGA ENTERPRISES"
    header[48:56] = b"JUE     "
    product = f"BM-{index:06d}".encode()
    header[64:64 + len(product)] = product
    title = f"Benchmark {stem}".encode()
    header[128:128 + len(title)] = title
    pvd = bytearray(2048)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[156] = 34
    struct.pack_into("<I", pvd, 158, 20)
    struct.pack_into(">I", pvd, 162, 20)
    struct.pack_into("<I", pvd, 166, 2048)
    struct.pack_into(">I", pvd, 170, 2048)
    pvd[181] = 2
    with (root / f"{stem}.bin").open("xb") as raw:
        raw.write(header)
        raw.seek(16 * 2048)
        raw.write(pvd)
        raw.seek(20 * 2048)
        raw.write(bytes(2048))
        raw.truncate(args.track_mib * 1024 * 1024)
    (root / f"{stem}.cue").write_text(
        f'FILE "{stem}.bin" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n'
    )
files = list(root.iterdir())
summary = {
    "games": args.count,
    "track_logical_bytes": args.track_mib * 1024 * 1024,
    "total_logical_bytes": sum(path.stat().st_size for path in files),
    "allocated_bytes": sum(path.stat().st_blocks * 512 for path in files),
    "embedded_artwork": False,
    "data_generated_before_timing": True,
}
(root / "fixture.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
