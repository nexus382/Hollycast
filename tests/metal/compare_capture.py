#!/usr/bin/env python3
"""Detect missing rendered surfaces in synchronized backend captures (Pillow required)."""
import argparse
from PIL import Image, ImageChops, ImageStat

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("reference")
parser.add_argument("candidate")
parser.add_argument("--max-missing", type=float, default=0.02,
                    help="Maximum fraction of reference-visible pixels replaced by near-black")
args = parser.parse_args()
reference = Image.open(args.reference).convert("RGB")
candidate = Image.open(args.candidate).convert("RGB")
if reference.size != candidate.size:
    raise SystemExit(f"Different image sizes: {reference.size} vs {candidate.size}")
reference_pixels = reference.tobytes()
candidate_pixels = candidate.tobytes()
visible = 0
missing = 0
for i in range(0, len(reference_pixels), 3):
    if max(reference_pixels[i:i+3]) > 40:
        visible += 1
        missing += max(candidate_pixels[i:i+3]) < 8
if not visible:
    raise SystemExit("FAIL: no visible reference surfaces to assess")
fraction = missing / visible
frame_fraction = missing / (reference.width * reference.height)
mae = sum(ImageStat.Stat(ImageChops.difference(reference, candidate)).mean) / 3
print(f"Missing visible surfaces: {fraction:.3%} ({frame_fraction:.3%} of the frame); "
      f"RGB mean absolute error: {mae:.3f}/255")
if fraction > args.max_missing:
    raise SystemExit("FAIL: visible reference surfaces are absent")
print("PASS: missing-surface threshold")
