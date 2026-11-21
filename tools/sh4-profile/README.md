# SH-4 guest-loop profiling

Checked-in JSON summaries use `<checkout>`, `~`, and `<local-host>` in place of
machine-specific paths and host names. Original captures remain local, and their
recorded SHA-256 hashes and measurements are unchanged. Regeneration writes the
local paths for the machine performing the capture.

The profiler adds observational counters and host samples; it must not change guest
cycle deductions, scheduler intervals, SH-4 state, or the replay inputs. See
[CAPTURE.md](CAPTURE.md) for the Release/JIT capture and overhead comparison workflow.

Analyze one capture:

```sh
python3 tools/sh4-profile/analyze.py capture.json \
  --json loops.json --markdown loops.md --top 10
python3 -m unittest discover -s tools/sh4-profile -v
```

`loops.md` includes the top ten loops' addresses, captured disassembly, block
execution counts, backedge traversals, CPU sample shares, and any fallback/state
synchronization evidence. `loops.json` retains every loop and all its block records.
A rank is based on **exclusive** CPU samples: a sample inside a nested loop belongs
only to the innermost loop. Inclusive samples describe nesting and must not be
summed. CPU shares use all runnable emulation-thread samples, including unmapped
ones, as the denominator. The accounting buckets must add to that denominator.

The analysis finds dominance backedges on the observed intra-procedural graph and
unions natural loops with a common header. Irreducible cycles are reported as SCCs;
they have no unique header or iteration count. Overlapping loops that lack a unique
innermost owner put their samples in the explicit ambiguous bucket. Calls, returns,
exceptions and scheduler boundaries must be marked, so they do not create false
loops. An unobserved edge can change the inferred loop structure; this is a profile
of this capture, not a proof about all executions.

## Capture interchange format

All addresses may be JSON integers or `0x` hexadecimal strings. Counts use integers.
Block IDs must uniquely identify a compiled generation. `rx_end` and `retired_ns` are
exclusive; native sample timestamps and lifecycle timestamps must share a monotonic
host-clock epoch.

```json
{
  "metadata": {
    "sample_period_ns": 1000000,
    "count_mode": "exact block/edge counters",
    "scene": "record actual game, guest-cycle window, replay/media hashes here"
  },
  "entries": ["generation-1"],
  "blocks": [{
    "id": "generation-1",
    "pc": "0x8c010000",
    "rx_begin": "0x100001000",
    "rx_end": "0x100001080",
    "created_ns": 100,
    "retired_ns": null,
    "executions": 1000,
    "disassembly": [{"pc": "0x8c010000", "opcode": "0x0009", "text": "nop"}],
    "profile_spans": [{"host_offset": 0, "host_size": 16, "guest_pc": "0x8c010000", "kind": 0, "target": 0}],
    "fallback": ["static annotation: interpreter fallback site at 0x8c010002"],
    "sync": ["static annotation: floating-point state synchronization site"]
  }],
  "edges": [{"from": "generation-1", "to": "generation-1", "count": 999, "kind": "branch"}],
  "samples": [{"host_ns": 200, "pcs": ["0x100001004"], "runnable": true}]
}
```

`entries` records the first block and entries following trace discontinuities.
Additional zero-indegree/source SCCs are treated conservatively as possible entries.
Excluded `kind` values are `call`, `return`, `exception`, `scheduler`, and
`discontinuity`. Missing `kind` means `branch`.

Native `pcs` are ordered leaf-to-root. The analyzer maps the first JIT frame to the
range live at that timestamp and never resolves ambiguous live generation overlaps.
Non-leaf frames are return addresses and are adjusted by one byte before range/span
matching. Mapping the leaf is direct JIT attribution; mapping a caller is inferred
attribution of helper/runtime work to its calling guest block. Preserve those
categories when interpreting CPU cost. Truncated stacks and unavailable native
frames remain unmapped. Reused executable addresses are resolved with lifecycle
timestamps, not the final block map.

Spans use offsets relative to `rx_begin`: `kind=0` is the generated SHIL opcode,
`1` a runtime-call setup/branch, `2` a register load, and `3` a register store.
Nested specialized spans classify the narrow generated sequence. A runtime-call
span does not itself measure time spent in the callee; a sample in the callee needs
a valid JIT return frame for caller attribution. `target_symbol` is optional.
`guest_pc_samples` and `cpu_sample_categories` in analyzed block records retain the
attribution detail. These are statistical estimates, including CPU milliseconds
calculated from sample period, not cycle-accurate instruction timings.

For an external profiler that has already mapped samples, use a summary object
instead of the raw `samples` array:

```json
"samples": {"total": 10000, "cpu_period_ns": 1000000}
```

Each block then supplies its `cpu_samples` weight. The total must include unmapped
and outside-loop samples; it must not be smaller than the sum of mapped weights.
A missing sample period leaves CPU milliseconds unknown. Weighted samples may use
`weight`, but the producer must document the sampling design and units.

Fallback/synchronization arrays are static annotations. A dictionary can instead
supply counts, but the producer must label exact counts versus execution-weighted
site estimates in metadata and in the key names. Neither static site counts nor
execution counts establish that a fallback is expensive. Recommend the first
optimization from the highest exclusive CPU contributor and its measured helper or
state-access cost, with capture coverage and overhead explicitly stated.

The in-process capture may emit `to_pc` instead of `to` on an edge. Native samples
are then mapped to generations first, and blocks with identical guest PC and
captured instruction addresses/opcodes are combined for the guest CFG. Edges resolve by a unique
captured guest-code version at `to_pc`. Missing or ambiguous versions are excluded
and their execution counts appear as `unresolved_target_edge_executions`; inspect
that coverage before trusting loop structure. Call and exception targets become
CFG roots. Shared SH-4 callees form separate regions; their work is not folded into
every caller loop. `generation_ids` preserves the aggregation provenance.

When `metadata.thread_cpu_ns` is present, estimated exclusive CPU milliseconds are
that measured thread-CPU duration multiplied by the sample share. Otherwise the
estimate uses sample count times the nominal period. The report records the method.
Sampling fractions and mapping coverage are estimates even with an exact CPU clock.

For the least biased ranking, capture a `sample` run and a separate `counts` run of
the same deterministic scene, then join them:

```sh
python3 tools/sh4-profile/analyze.py sample-profile.json \
  --counts counts-profile.json --json loops.json --markdown loops.md
```

CPU weights come only from the sample run; execution counts and CFG come from the
counted replay. The join uses identical guest PC plus instruction addresses/opcodes, sums generations
once, and records both metadata sets, boundary equality, and unmatched sampled code.
Verify replay inputs/media, cycle bounds and screenshots independently; equal code
and boundary metadata alone do not prove identical paths. Do not interpret the
result as a simultaneous exact execution trace and CPU measurement.

`call_continuation_estimate` edges represent a structural return-to-next-block hint
at a guest call. Their counts are upper bounds on returning calls, not measured
returns. They preserve loops spanning calls without including callees; when such an
edge is a backedge, `backedge_count_complete` is false and the upper bound is shown
separately. Kind `4` spans identify interpreter fallback bodies; generic kind `1`
spans identify runtime helper calls. Profiling helper samples are labeled separately
when their symbol is available. Allocator load/store spans cover only the explicitly
instrumented subset of state synchronization.

Disassembler register-value annotations describe compilation-time state, not each
execution. Code identity uses captured instruction addresses and opcodes; changing
register annotations across runs does not split otherwise identical guest code.

The analyzer also identifies `two_block_cycle` regions from two reciprocal **measured**
branch edges. These can reveal a hot short cycle inside a larger multi-entry SCC
where no single block dominates the other. Their two block sets participate in the
same innermost exclusive ownership scheme as natural loops; the parent SCC keeps
only residual samples. Equal-size overlapping cycles go to the ambiguous bucket.
Both directional edge counts are exact observations; their minimum is only an upper
bound on uninterrupted completed two-edge cycles because aggregate edge counts do
not retain traversal order. Estimated call continuations never establish these
cycles. `loop_rank` ranks natural loops and observed two-block cycles together;
`natural_loop_rank` ranks only natural loops. SCC residual rows remain distinct.

Pool repeated captures reproducibly after `capture.py` finishes:

```sh
python3 tools/sh4-profile/summarize.py /path/to/capture.json --output pooled-results.json
```

The default pairs each sample-only replay with the nearest counted replay in capture
order (earlier counted run breaks a distance tie). Override with repeated
`--pair /path/to/sample-profile.json /path/to/count-profile.json` arguments. The tool
writes each paired analyzer report next to `capture.json`, then pools by kind,
representative/header PC and member instruction addresses/opcodes. Generation IDs
and compilation-time register comments do not define cross-run identity. Samples
are summed; exact per-replay execution/backedge counts remain ranges, not three-run
sums. It preserves sample/count provenance, block/edge count fingerprints, mapping
coverage, native leaf symbols, and top-ten disassembly. Closely ranked small loops
can exchange positions through sampling noise; per-run estimates are retained.

The checked-in [RESULTS.md](RESULTS.md) is a curated interpretation of
[results.json](results.json). Reproduce its data on this machine with:

```sh
python3 tools/sh4-profile/summarize.py \
  .cache/sh4-profile-captures/sh4-lghycjxt/capture.json \
  --output tools/sh4-profile/results.json
```
