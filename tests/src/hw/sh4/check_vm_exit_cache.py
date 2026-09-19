"""Guard the compile-once exit classification (run directly with Python 3)."""

from pathlib import Path
import re


source = (Path(__file__).resolve().parents[4] / "core/hw/sh4/dyna/shil_vm.cpp").read_text()
compile_body = source.split("void compile(", 1)[1].split("void reset()", 1)[0]
assert len(re.findall(r"shilVmExitKind\(", source)) == 1, "classify only at compilation"
assert "irBlock->exitKind = shilVmExitKind(block->BlockType);" in compile_body
assert compile_body.index("irBlock->exitKind =") < compile_body.index("if (disp == AotDispatch::Hit)")
assert "const ShilVmExitKind kind = irBlock->exitKind;" in source
assert "endBlock(ctx, block, irBlock->exitKind, nullptr);" in source
print("PASS: exit kind classified once at compile time; dispatch and idle probe reuse it")
