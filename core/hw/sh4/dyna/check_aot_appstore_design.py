#!/usr/bin/env python3
"""Machine-checkable done criterion for the App-Store AOT design investigation.

Exit 0 only when AOT_APPSTORE_DESIGN.md (sibling of this script) contains every
required section heading and every required decision line. No repository trusted
scripts are consulted; this file is the sole gate for the investigation slice.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

DESIGN = Path(__file__).with_name("AOT_APPSTORE_DESIGN.md")

# Exact Markdown H2 headings the design must include (order free).
REQUIRED_SECTIONS = (
	"## DONE_CRITERION",
	"## CONSTRAINT",
	"## GAP_BUDGET",
	"## WHAT_TO_AOT",
	"## WHEN",
	"## HOW_PIPELINE",
	"## SMC_INVALIDATION",
	"## RECOMMENDATION",
	"## FIRST_IMPLEMENTATION_SLICE",
	"## REJECTED_ALTERNATIVES",
)

# Each key must appear as a line starting with `KEY: ` and a non-empty value.
REQUIRED_KEYS = (
	"LEGALITY",
	"BASELINE_JITLESS_FPS",
	"BASELINE_JIT_FPS",
	"BASELINE_RATIO",
	"TARGET_RATIO",
	"REQUIRED_SPEEDUP",
	"AOT_UNIT",
	"EMIT_TIME",
	"SHIP_FORM",
	"FALLBACK",
	"SMC_STRATEGY",
	"CHOSEN_APPROACH",
	"FIRST_SLICE",
)


def fail(msg: str) -> int:
	print(f"FAIL: {msg}", file=sys.stderr)
	return 1


def main() -> int:
	if not DESIGN.is_file():
		return fail(f"missing design file: {DESIGN}")

	text = DESIGN.read_text(encoding="utf-8")
	if not text.strip():
		return fail("design file is empty")

	missing_sections = [s for s in REQUIRED_SECTIONS if s not in text]
	if missing_sections:
		return fail("missing sections: " + ", ".join(missing_sections))

	for key in REQUIRED_KEYS:
		# Allow optional bold markers around the key.
		pat = re.compile(rf"(?m)^\*?\*?{re.escape(key)}\*?\*?:\s+\S")
		if not pat.search(text):
			return fail(f"missing decision key line `{key}: <value>`")

	# Spot-check that the recommendation is the legal native-blob path, not IR cache.
	chosen = None
	for line in text.splitlines():
		if line.lstrip("*").startswith("CHOSEN_APPROACH:"):
			chosen = line.split(":", 1)[1].strip().lower()
			break
	if chosen is None:
		return fail("CHOSEN_APPROACH line unreadable")
	if "native" not in chosen and "aot" not in chosen:
		return fail("CHOSEN_APPROACH must name a native/AOT approach")
	if "ir-cache" in chosen or "non-executable ir" in chosen:
		return fail("CHOSEN_APPROACH must not be a non-executable IR cache")

	print(f"PASS: {DESIGN.name} satisfies {len(REQUIRED_SECTIONS)} sections and {len(REQUIRED_KEYS)} keys")
	return 0


if __name__ == "__main__":
	sys.exit(main())
