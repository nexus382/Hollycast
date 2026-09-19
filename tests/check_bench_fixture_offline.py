#!/usr/bin/env python3
"""Independent, dump-free ARM64 compilation, emission, behavior and ABI gate.

Run with python3 tests/check_bench_fixture_offline.py on an ARM64 host with
clang++. No game, configured build tree, main ref, or runtime JSON is needed.
"""
from pathlib import Path
import copy
import json
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REC = ROOT / 'core/rec-ARM64'


def run(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode:
        print(result.stdout + result.stderr)
        result.check_returncode()
    return result


def main():
    cmake = (ROOT / 'CMakeLists.txt').read_text()
    for option in ('FLYCAST_AOT_OFFLINE_TOOLING', 'FLYCAST_AOT_BENCH_FIXTURE'):
        assert re.search(r'option\(' + option + r' "[^"]+" OFF\)', cmake), option
    assert 'file(GLOB FLYCAST_AOT' not in cmake
    fixture_section = cmake[cmake.index('if(FLYCAST_AOT_BENCH_FIXTURE)'):]
    assert 'aot_offline_bench_table.cpp' not in cmake[:cmake.index('if(FLYCAST_AOT_BENCH_FIXTURE)')]
    assert 'aot_sh4_0000beef_1083127669.S' in fixture_section
    table = (ROOT / 'core/hw/sh4/dyna/aot_table.h').read_text()
    assert 'g_aotTable = nullptr' in table and 'g_aotTableCount = 0' in table
    assert [p.name for p in REC.rglob('*.json')] == ['bench_fixture.json']
    for p in REC.rglob('*'):
        if p.is_file():
            assert 'crazy_taxi' not in p.read_text(errors='replace').lower(), str(p)
    rec = (REC / 'rec_arm64.cpp').read_text()
    assert rec.index('#include "hw/sh4/dyna/shil_vm_smc.h"') < rec.index('\nusing namespace vixl::aarch64')
    for pair in ('x19, x20', 'x21, x22', 'x23, x24', 'x25, x26', 'x27, x28',
                 'x29, x30', 'd8, d9', 'd10, d11', 'd12, d13', 'd14, d15'):
        assert 'Stp(' + pair in rec and 'Ldp(' + pair in rec, pair
    print('PASS: source no-dump/default-empty configuration and ARM64 frame checks')
    assert platform.machine().lower() in ('arm64', 'aarch64'), 'native ARM64 execution required'
    flags = ['clang++', '-std=c++17', '-Wno-deprecated-declarations',
             '-Wno-deprecated-literal-operator', '-I' + str(ROOT / 'core'),
             '-I' + str(ROOT / 'core/deps'), '-I' + str(ROOT / 'core/deps/nowide/include')]
    with tempfile.TemporaryDirectory(prefix='aot-fixture-') as tmp:
        tmp = Path(tmp)
        run(flags + ['-I' + str(ROOT / 'core/deps/vixl'), '-DVIXL_INCLUDE_TARGET_AARCH64',
                     '-DVIXL_INCLUDE_SIMULATOR_AARCH64', '-c', str(REC / 'rec_arm64.cpp'),
                     '-o', str(tmp / 'rec.o')])
        print('PASS: ARM64 rec_arm64.cpp object compilation')
        run(flags + ['-DAOT_OFFLINE_EMITTER_ONLY', str(REC / 'arm64_aot_offline.cpp'),
                     str(REC / 'aot_offline_main.cpp'), '-o', str(tmp / 'emit')])
        run(flags + ['-c', str(REC / 'arm64_aot_offline.cpp'), '-o', str(tmp / 'runtime.o')])
        symbols = run(['nm', '-u', str(tmp / 'runtime.o')]).stdout
        assert 'ifstream' not in symbols and 'json' not in symbols and 'EmitBenchFixture' not in symbols
        # Exercise CMake's real source-selection block in isolation from unrelated
        # application dependency submodules, including the opt-in architecture guard.
        start = cmake.index('# Offline emission is developer tooling.')
        end = cmake.index('if(("x86" IN_LIST', start)
        (tmp / 'empty.cpp').write_text('int empty;\n')
        (tmp / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.24)\nproject(probe LANGUAGES CXX)\n'
            'add_library(probe STATIC empty.cpp)\n'
            f'set(CMAKE_CURRENT_SOURCE_DIR "{ROOT}")\n'
            f'set_property(TARGET probe PROPERTY SOURCE_DIR "{ROOT}")\n'
            + cmake[start:end].replace('core/rec-ARM64/', str(REC) + '/')
            + '\nget_target_property(selected probe SOURCES)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${selected}")\n')
        for enabled in (False, True):
            build = tmp / ('with-fixture' if enabled else 'empty-build')
            run(['cmake', '-S', str(tmp), '-B', str(build), '-DARCHITECTURE=arm64',
                 '-DFLYCAST_AOT_OFFLINE_TOOLING=ON',
                 '-DFLYCAST_AOT_BENCH_FIXTURE=' + ('ON' if enabled else 'OFF')])
            selected = (build / 'selected.txt').read_text()
            assert ('aot_offline_bench_table.cpp' in selected) == enabled
            assert ('aot_sh4_0000beef_1083127669.S' in selected) == enabled
            assert '.json' not in selected and 'aot_offline_main.cpp' not in selected
        print('PASS: CMake default corpus empty; synthetic table/assembly require explicit opt-in')
        source = REC / 'aot_offline/bench_fixture.json'
        fixture = json.loads(source.read_text())
        # Neither a special path nor a title identity is required for emission.
        renamed = tmp / 'synthetic.json'
        renamed.write_text(source.read_text())
        generated = Path(run([str(tmp / 'emit'), str(renamed), str(tmp)]).stdout.strip())
        assert generated.read_bytes() == (REC / 'aot_offline' / generated.name).read_bytes()
        for field, value in [('sh4_bytes', '42611c311224f'), ('sh4_bytes', '4261zz1c311224'),
                             ('sh4_bytes', '0900'), ('sh4_hash', 0), ('pc', -1),
                             ('guest_cycles', 0), ('BlockType', 1), ('pc', '0x1garbage')]:
            invalid = copy.deepcopy(fixture)
            invalid['blocks'][0][field] = value
            renamed.write_text(json.dumps(invalid))
            result = subprocess.run([str(tmp / 'emit'), str(renamed), str(tmp)], capture_output=True)
            assert result.returncode != 0, (field, value)
            assert generated.read_bytes() == (REC / 'aot_offline' / generated.name).read_bytes()
        print('PASS: deterministic offline emission; invalid inputs rejected; runtime object has no JSON/file input')
        prefix = '_' if platform.system() == 'Darwin' else ''
        asm = ['.text', '.p2align 2', '.globl ' + prefix + 'probe', prefix + 'probe:', 'sub sp, sp, #176']
        pairs = [('x' + str(i), 'x' + str(i + 1)) for i in range(19, 31, 2)]
        pairs += [('d' + str(i), 'd' + str(i + 1)) for i in range(8, 16, 2)]
        for i, (a, b) in enumerate(pairs):
            asm.append(f'stp {a}, {b}, [sp, #{i * 16}]')
        for i in range(19, 30):
            asm.append(f'mov x{i}, #{100 + i}')
        for i in range(8, 16):
            asm += [f'mov x9, #{200 + i}', f'fmov d{i}, x9']
        asm += ['bl ' + prefix + 'aot_sh4_0000beef_1083127669', 'str w0, [sp, #160]']
        for i in range(19, 30):
            asm += [f'cmp x{i}, #{100 + i}', 'b.ne 1f']
        for i in range(8, 16):
            asm += [f'fmov x9, d{i}', f'cmp x9, #{200 + i}', 'b.ne 1f']
        asm += ['ldr w0, [sp, #160]', 'b 2f', '1:', 'mov w0, #256', '2:']
        for i, (a, b) in enumerate(pairs):
            asm.append(f'ldp {a}, {b}, [sp, #{i * 16}]')
        asm += ['add sp, sp, #176', 'ret']
        (tmp / 'probe.S').write_text('\n'.join(asm) + '\n')
        (tmp / 'probe.cpp').write_text(r'''
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/aot_table.h"
#include <cassert>
#include <cstring>
#include <cstdio>
extern "C" int probe(void *);
static u8 code[] = {0x42,0x61,0x1c,0x31,0x12,0x24};
static u32 memory = 21, reads = 0, writes = 0;
u8 *GetMemPtr(u32 addr, u32 size) { return addr == 0xbeef && size == sizeof(code) ? code : nullptr; }
static u32 read32(u32 addr) { assert(addr == 0x1000); ++reads; return memory; }
static void write32(u32 addr, u32 value) { assert(addr == 0x1000); ++writes; memory = value; }
ReadMem8Func ReadMem8 = nullptr;
ReadMem16Func ReadMem16 = nullptr;
ReadMem32Func ReadMem32 = read32;
ReadMem64Func ReadMem64 = nullptr;
WriteMem8Func WriteMem8 = nullptr;
WriteMem16Func WriteMem16 = nullptr;
WriteMem32Func WriteMem32 = write32;
WriteMem64Func WriteMem64 = nullptr;
int main() {
    assert(g_aotTableCount == 0 && aotLookup(0xbeef, 0) == nullptr);
    Sh4Context ctx{};
    ctx.r[4] = 0x1000; ctx.pc = 0xbeef; ctx.cycle_counter = 100;
    assert(probe(&ctx) == 0);
    assert(ctx.r[1] == 42 && memory == 42 && ctx.pc == 0xbef5 && ctx.cycle_counter == 97);
    assert(reads == 1 && writes == 1);
    code[0] ^= 1;
    Sh4Context before = ctx;
    assert(probe(&ctx) == 1);
    assert(memcmp(&ctx, &before, sizeof(ctx)) == 0 && memory == 42 && reads == 1 && writes == 1);
    puts("PASS: native fixture success=0 and SMC-mismatch=1; guest effects correct; x19-x29/d8-d15/SP preserved on both returns");
}
''')
        run(flags + ['-DTARGET_SHIL_VM', str(tmp / 'probe.cpp'), str(tmp / 'probe.S'),
                     str(tmp / 'runtime.o'), str(generated), '-o', str(tmp / 'probe')])
        print(run([str(tmp / 'probe')]).stdout.strip())
        # Registration is also executable evidence: only an explicitly linked
        # developer fixture supplies a native entry to the table.
        probe_source = (tmp / 'probe.cpp').read_text().replace(
            'assert(g_aotTableCount == 0 && aotLookup(0xbeef, 0) == nullptr);',
            'assert(g_aotTableCount == 1 && aotLookup(0xbeef, 0)->nativeEntry != nullptr);')
        (tmp / 'probe.cpp').write_text(probe_source)
        run(flags + ['-DTARGET_SHIL_VM', str(tmp / 'probe.cpp'), str(tmp / 'probe.S'),
                     str(tmp / 'runtime.o'), str(generated), str(REC / 'aot_offline_bench_table.cpp'),
                     '-o', str(tmp / 'registered-probe')])
        run([str(tmp / 'registered-probe')])
        print('PASS: explicitly linked developer fixture registers exactly one native entry')


if __name__ == '__main__':
    main()
