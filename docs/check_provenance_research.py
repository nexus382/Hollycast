"""Check report completeness and optionally retrieve pinned primary evidence."""

import pathlib
import re
import sys
p = pathlib.Path(__file__).resolve().parent / 'Provenance JITless Research.md'
assert p.is_file(), 'research report missing'
s = p.read_text()
for section in ['Scope and pinned sources', 'Core findings', 'HollyCast comparison', 'Recommendations', 'Performance acceptance', 'Limitations']:
    assert f'## {section}' in s, section
links = re.findall(r'https://github.com/[^\s)]+/blob/[0-9a-f]{40}/[^\s)]+', s)
assert len(set(links)) >= 10, 'need 10 unique immutable source links'
for term in ['Flycast', 'PPSSPP', 'Dolphin', 'Mupen', 'Mednafen', '75%', 'runtime code generation', 'title-specific', '8.86%', '1.333']:
    assert term in s, term
assert 'not measured' in s
assert s.count('| Confirmed') >= 4, 'need >=4 source-confirmed execution-path rows'
print(f'PASS: research report; 6 required sections; {len(set(links))} immutable source links; >=4 confirmed execution paths; constraints and quantitative acceptance present')

if '--fetch' in sys.argv:
    import concurrent.futures
    import urllib.request

    signatures = {
        '/CachedInterpreterEmitter.h': 'Common::CodeBlock<CachedInterpreterEmitter, false>',
        '/CachedInterpreter.cpp': '[[clang::musttail]]',
        '/CachedInterpreterIR.cpp': 'ICUBE_IR_THREADED_DISPATCH',
        '/IRInterpreter.cpp': 'u32 IRInterpret(',
        '/IRJit.cpp': 'IRInterpret(mips_',
        '/driver_jitless.cpp': '*(RuntimeBlockInfo**)code_ptr = block;',
        '/PVFlycastEmuCore.swift': 'return .requiredOrCrash',
        '/local-dylibs.sh': 'Provenance-Emu/provenance-dylibs',
        '/psx/cpu.cpp': 'goto *op_goto_table[opf]',
    }

    def fetch(link):
        url = link.split('#', 1)[0]
        raw = url.replace('https://github.com/', 'https://raw.githubusercontent.com/', 1).replace('/blob/', '/', 1)
        with urllib.request.urlopen(raw, timeout=30) as response:
            source = response.read().decode('utf-8')
        assert source.strip(), f'empty source: {url}'
        for suffix, needle in signatures.items():
            if url.endswith(suffix):
                assert needle in source, f'{suffix}: missing source signature {needle!r}'
        return url

    urls = sorted({link.split('#', 1)[0] for link in links})
    for suffix in signatures:
        assert any(url.endswith(suffix) for url in urls), f'missing evidence source {suffix}'
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        fetched = list(executor.map(fetch, urls))
    print(f'PASS: retrieved {len(fetched)} pinned source files; {len(signatures)} execution/packaging signatures verified')
