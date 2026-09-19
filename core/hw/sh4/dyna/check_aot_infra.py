#!/usr/bin/env python3
"""Machine-checkable done criterion for App-Store-legal AOT infrastructure.

Independent of CMake/gtest/CI trusted scripts. Exit 0 only when the scaffolding
this slice must land is present and stays App-Store-legal (no on-device emit).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[3]  # .../HollyCast
SHIL_VM = ROOT / "shil_vm.cpp"
AOT_TABLE = ROOT / "aot_table.h"
HOT_DUMP = ROOT / "aot_hot_dump.h"
ARTIFACT = ROOT / "crazy_taxi_hot_blocks.json"
TEST_DIR = REPO / "tests" / "src" / "hw" / "sh4"

FORBIDDEN = ("PROT_EXEC", "MAP_JIT", "PROT_WRITE | PROT_EXEC", "RWX", "mprotect")


def fail(msg: str) -> int:
	print(f"FAIL: {msg}", file=sys.stderr)
	return 1


def main() -> int:
	for path in (AOT_TABLE, HOT_DUMP, ARTIFACT, SHIL_VM):
		if not path.is_file():
			return fail(f"missing required file: {path.relative_to(REPO)}")

	table = AOT_TABLE.read_text(encoding="utf-8")
	for needle in ("aotLookup", "AotTableEntry", "expectedSmcHash", "cfgBits", "aotClassify"):
		if needle not in table:
			return fail(f"{AOT_TABLE.name} missing `{needle}`")

	dump = HOT_DUMP.read_text(encoding="utf-8")
	for needle in ("aotHotNoteVisit", "aotHotDumpTopN", "sh4_hash", "guest_cycles", "BlockType", "targets"):
		if needle not in dump:
			return fail(f"{HOT_DUMP.name} missing `{needle}`")

	artifact = ARTIFACT.read_text(encoding="utf-8")
	for key in ('"pc"', '"sh4_hash"', '"sh4_bytes"', '"guest_cycles"', '"BlockType"', '"targets"'):
		if key not in artifact:
			return fail(f"{ARTIFACT.name} schema missing key {key}")

	shil = SHIL_VM.read_text(encoding="utf-8")
	if '#include "aot_table.h"' not in shil and "#include <aot_table.h>" not in shil:
		return fail("shil_vm.cpp does not include aot_table.h")
	if "aotClassify" not in shil and "aotLookup" not in shil:
		return fail("shil_vm.cpp never consults the AOT table")
	# Encode must sit after the AOT consult so miss/SMC-mismatch fall back to SHIL VM.
	encode_at = shil.find("shilVmEncode")
	consult_at = shil.find("aotClassify")
	if consult_at < 0:
		consult_at = shil.find("aotLookup")
	if encode_at < 0 or consult_at < 0 or consult_at > encode_at:
		return fail("AOT table must be consulted before shilVmEncode in shil_vm.cpp")

	# No new executable mapping / JIT emit in AOT scaffolding files.
	for path in (AOT_TABLE, HOT_DUMP):
		text = path.read_text(encoding="utf-8")
		for bad in FORBIDDEN:
			if bad in text:
				return fail(f"{path.name} must not mention {bad}")

	# Tests cover miss and SMC-mismatch fallback (existing test translation unit).
	test_hits = []
	for path in sorted(TEST_DIR.glob("*.cpp")):
		text = path.read_text(encoding="utf-8")
		if "aotClassify" in text or "AotDispatch" in text or "aotLookup" in text:
			test_hits.append(path.name)
			if "Miss" not in text and "miss" not in text:
				return fail(f"{path.name} touches AOT API but has no miss coverage")
			if "Smc" not in text and "SMC" not in text and "smc" not in text:
				return fail(f"{path.name} touches AOT API but has no SMC-mismatch coverage")
	if not test_hits:
		return fail("no test under tests/src/hw/sh4/ exercises the AOT lookup API")

	print(
		f"PASS: AOT infra present (table={AOT_TABLE.name}, dump={HOT_DUMP.name}, "
		f"artifact={ARTIFACT.name}, tests={','.join(test_hits)})"
	)
	return 0


if __name__ == "__main__":
	sys.exit(main())
