"""Check an explicit revision's VM boundary, independently of build scripts.

Usage: python3 tests/src/hw/sh4/check_vm_no_dump.py <git revision>
This is a structural gate, complemented by the runtime fallback tests.
"""

from pathlib import Path
import re
import subprocess
import sys


root = Path(__file__).resolve().parents[4]
revision = sys.argv[1]


def source(path):
    return subprocess.check_output(
        ["git", "show", f"{revision}:{path}"], cwd=root, text=True
    )


vm = source("core/hw/sh4/dyna/shil_vm.cpp")
table = source("core/hw/sh4/dyna/aot_table.h")
cmake = source("CMakeLists.txt")
compile_body = vm.split("void compile(", 1)[1].split("void reset()", 1)[0]
assert vm.count("shilVmExitKind(block->BlockType)") == 1, "exit classification must occur once"
assert "irBlock->exitKind = shilVmExitKind(block->BlockType);" in compile_body
assert "const ShilVmExitKind kind = irBlock->exitKind;" in vm
assert "endBlock(ctx, block, irBlock->exitKind, nullptr);" in vm
assert "g_aotTable = nullptr;" in table and "g_aotTableCount = 0;" in table
assert "if (tab == nullptr || n == 0)\n\t\treturn nullptr;" in table
assert "if (e == nullptr || e->nativeEntry == nullptr)\n\t\treturn AotDispatch::Miss;" in table
hit = compile_body.split("if (disp == AotDispatch::Hit)", 1)[1]
assert re.search(r"return;\s*}\s*irBlock->program = shilVmEncode\(block->oplist\);", hit)
assert "static bool s_aotHotProfiling = false;" in vm
assert not re.search(r"ifstream|fopen|freopen|fread|json::parse|HOLLYCAST_AOT_HOT_DUMP", vm)
assert not re.search(r"crazy_taxi_hot_blocks|AOT_HOT_DUMP", cmake)
code = re.sub(r"/\*.*?\*/|//[^\n]*", "", vm + table, flags=re.DOTALL)
assert not re.search(r"MAP_JIT|PROT_EXEC|mprotect\s*\(", code)
print(f"PASS: {revision}: cached exits; empty-table VM fallback; no dump input or executable-memory API in VM/table")
