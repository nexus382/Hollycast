#!/usr/bin/env python3
"""Analyze an executed SH-4 CFG. See README.md for the capture interchange format."""
import argparse
import bisect
import json
from collections import defaultdict
from pathlib import Path


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def strongly_connected(nodes, successors):
    # Iterative Kosaraju avoids recursion depth limits on real games.
    seen, order = set(), []
    for root in sorted(nodes):
        if root in seen:
            continue
        seen.add(root)
        stack = [(root, iter(successors[root]))]
        while stack:
            node, children = stack[-1]
            child = next(children, None)
            if child is None:
                order.append(node)
                stack.pop()
            elif child not in seen:
                seen.add(child)
                stack.append((child, iter(successors[child])))
    predecessors = {n: set() for n in nodes}
    for node in nodes:
        for child in successors[node]:
            predecessors[child].add(node)
    seen, components = set(), []
    for root in reversed(order):
        if root in seen:
            continue
        component, stack = set(), [root]
        seen.add(root)
        while stack:
            node = stack.pop()
            component.add(node)
            for child in predecessors[node] - seen:
                seen.add(child)
                stack.append(child)
        components.append(component)
    return components


def find_loops(nodes, edges, entries):
    successors = {n: set() for n in nodes}
    predecessors = {n: set() for n in nodes}
    for source, target in edges:
        successors[source].add(target)
        predecessors[target].add(source)
    # An artificial entry reaches every source SCC, so an unseen program entry
    # cannot manufacture dominance inside an otherwise isolated cycle.
    components = strongly_connected(nodes, successors)
    roots = set(entries)
    unknown_entry_components = set()
    for component in components:
        if not any(predecessors[n] - component for n in component):
            known = component & roots
            if not known:
                unknown_entry_components.add(frozenset(component))
            roots.update(known or component)
    ordered = sorted(nodes)
    bits = {n: 1 << i for i, n in enumerate(ordered)}
    all_bits = (1 << len(nodes)) - 1
    dominators = {n: (bits[n] if n in roots else all_bits) for n in nodes}
    changed = True
    while changed:
        changed = False
        for node in ordered:
            if node in roots:
                continue
            parents = predecessors[node]
            common = all_bits if parents else 0
            for parent in parents:
                common &= dominators[parent]
            value = bits[node] | common
            if value != dominators[node]:
                dominators[node] = value
                changed = True
    loops = {}
    for source, target in edges:
        if not (bits[target] & dominators[source]):
            continue
        members, pending = {source, target}, ([] if source == target else [source])
        while pending:
            node = pending.pop()
            for parent in predecessors[node] - members:
                members.add(parent)
                pending.append(parent)
        loop = loops.setdefault(target, {"kind": "natural", "header": target, "members": set(), "backedges": []})
        loop["members"].update(members)
        loop["backedges"].append((source, target))
    result = list(loops.values())
    # Keep irreducible cycles visible. Natural loops inside an irreducible SCC
    # remain children; the SCC gets only the residual samples.
    for component in components:
        cyclic = len(component) > 1 or any((n, n) in edges for n in component)
        if cyclic and not any(component == loop["members"] for loop in result):
            component_entries = {n for n in component if predecessors[n] - component} | (component & roots)
            if len(component_entries) > 1 or not any(component <= loop["members"] for loop in result):
                result.append({"kind": "cyclic_scc_unknown_entry" if frozenset(component) in unknown_entry_components else "irreducible_scc", "header": min(component), "members": component, "backedges": []})
    # Reciprocal executed edges prove a short cyclic path even when multiple
    # entries prevent dominance from identifying it as a natural loop. Marginal
    # edge counts do not prove uninterrupted iterations; retain both exact edge
    # traversals and only an upper bound on completed two-edge cycles.
    for source in sorted(nodes):
        for target in successors[source]:
            if source >= target or source not in successors[target] or not edges[source, target] or not edges[target, source]:
                continue
            members = {source, target}
            if any(loop["kind"] == "natural" and loop["members"] == members for loop in result):
                continue
            result = [loop for loop in result if not (loop["kind"] != "natural" and loop["members"] == members)]
            result.append({"kind": "two_block_cycle", "header": min(members), "members": members,
                           "backedges": [], "cycle_edges": [(source, target), (target, source)]})
    return result


def map_samples(capture, blocks):
    """Map raw native stacks to live JIT generations without recycling ambiguity."""
    starts = defaultdict(list)
    for key, block in blocks.items():
        if "rx_begin" in block and "rx_end" in block:
            starts[number(block["rx_begin"])].append((key, block))
    addresses = sorted(starts)
    prefix_max_end = []
    maximum_end = 0
    for address in addresses:
        maximum_end = max(maximum_end, *(number(block["rx_end"]) for _, block in starts[address]))
        prefix_max_end.append(maximum_end)
    mapped = defaultdict(float)
    categories = defaultdict(lambda: defaultdict(float))
    guest_samples = defaultdict(lambda: defaultdict(float))
    native_leaves = defaultdict(lambda: defaultdict(float))
    unmapped_leaves = defaultdict(float)
    direct = helper = unknown = excluded = 0.0
    for sample in capture.get("samples", []):
        weight = float(sample.get("weight", 1))
        if not sample.get("runnable", True):
            excluded += weight
            continue
        timestamp = number(sample["host_ns"])
        found = None
        for depth, pc in enumerate(sample.get("pcs", [])):
            # Non-leaf frames are return addresses, hence belong to the caller
            # instruction immediately before the recorded return address.
            address = number(pc) - (1 if depth else 0)
            index = bisect.bisect_right(addresses, address) - 1
            if index < 0:
                continue
            candidates = []
            while index >= 0 and prefix_max_end[index] > address:
                for key, block in starts[addresses[index]]:
                    retired = block.get("retired_ns")
                    if number(block["rx_begin"]) <= address < number(block["rx_end"]) and number(block.get("created_ns", 0)) <= timestamp and (retired is None or timestamp < number(retired)):
                        candidates.append(key)
                index -= 1
            if len(candidates) == 1:
                found = candidates[0]
                mapped[found] += weight
                if depth:
                    native_leaves[found][sample.get("leaf_symbol") or "unknown native leaf"] += weight
                relative = address - number(blocks[found]["rx_begin"])
                spans = [span for span in blocks[found].get("profile_spans", [])
                         if number(span["host_offset"]) <= relative < number(span["host_offset"]) + number(span["host_size"])]
                opcode_spans = [span for span in spans if span.get("kind") in (0, "opcode")]
                if opcode_spans:
                    opcode = min(opcode_spans, key=lambda span: number(span["host_size"]))
                    guest_samples[found][str(opcode["guest_pc"])] += weight
                fallback_span = any(span.get("kind") in (4, "interpreter_fallback") for span in spans)
                specialized = [span for span in spans if span.get("kind") not in (0, "opcode")]
                span = min(specialized, key=lambda span: number(span["host_size"])) if specialized else None
                kind = {1: "runtime_call", 2: "register_load", 3: "register_store", 4: "interpreter_fallback"}.get(span.get("kind"), span.get("kind")) if span else "unclassified"
                if fallback_span:
                    kind = "interpreter_fallback"
                if span and "sh4_loop_dynamic_edge" in str(span.get("target_symbol", "")):
                    kind = "profiling_overhead"
                category = ("callee_via_jit_return/" if depth else "jit_self/") + str(kind)
                if span and span.get("target"):
                    category += "/" + str(span.get("target_symbol", span["target"]))
                categories[found][category] += weight
                if depth:
                    helper += weight
                else:
                    direct += weight
                break
            if len(candidates) > 1:
                # Ambiguous generation overlap is never silently attributed.
                break
        if found is None:
            unknown += weight
            unmapped_leaves[sample.get("leaf_symbol") or "unknown native leaf"] += weight
    for key, block in blocks.items():
        block["cpu_samples"] = mapped[key]
        block["cpu_sample_categories"] = dict(categories[key])
        block["guest_pc_samples"] = dict(guest_samples[key])
        block["native_leaf_samples"] = dict(native_leaves[key])
    metadata = capture.get("metadata", {})
    return {"total": direct + helper + unknown, "mapped_direct_jit": direct,
            "mapped_via_jit_caller": helper, "unmapped": unknown,
            "excluded_non_runnable": excluded,
            "cpu_period_ns": metadata.get("sample_period_ns"),
            "unmapped_native_leaf_samples": dict(unmapped_leaves),
            "mapped_to_guest_instruction_samples": sum(sum(weights.values()) for weights in guest_samples.values()),
            "mapped_to_guest_block_only_samples": direct + helper - sum(sum(weights.values()) for weights in guest_samples.values())}


def aggregate_annotations(blocks, ids, field):
    counts, annotations = defaultdict(int), set()
    for key in ids:
        annotations.update(blocks[key].get(field + "_static_annotations", []))
        value = blocks[key].get(field, {})
        if isinstance(value, dict):
            for name, count in value.items():
                counts[name] += number(count)
        elif isinstance(value, list):
            annotations.update(str(item) for item in value)
        elif value:
            annotations.add(str(value))
    return {"producer_counts_or_estimates": dict(counts), "static_annotations": sorted(annotations)}


def merge_guest_generations(capture, generations):
    """Resolve native lifetimes first; then build an aggregate guest-code CFG."""
    grouped, group_ids, signatures = {}, {}, {}
    for generation, block in generations.items():
        signature = (number(block["pc"]), tuple((number(ins["pc"]), ("opcode", number(ins["opcode"])) if "opcode" in ins else ("text", ins["text"])) if isinstance(ins, dict) else ins for ins in block.get("disassembly", [])))
        if not block.get("disassembly"):
            signature += (generation,)
        if signature not in signatures:
            signatures[signature] = generation
            grouped[generation] = dict(block, executions=0, cpu_samples=0, cpu_sample_categories={}, guest_pc_samples={}, native_leaf_samples={}, generation_ids=[], fallback={}, sync={}, fallback_static_annotations=[], sync_static_annotations=[])
        group = signatures[signature]
        group_ids[generation] = group
        merged = grouped[group]
        merged["generation_ids"].append(generation)
        merged["executions"] += number(block.get("executions", 0))
        merged["cpu_samples"] += float(block.get("cpu_samples", 0))
        for field in ("cpu_sample_categories", "guest_pc_samples", "native_leaf_samples"):
            for key, weight in block.get(field, {}).items():
                merged[field][key] = merged[field].get(key, 0) + weight
        for field in ("fallback", "sync"):
            value = block.get(field, {})
            if isinstance(value, dict):
                for key, count in value.items():
                    merged[field][key] = merged[field].get(key, 0) + number(count)
            elif isinstance(value, list):
                merged[field + "_static_annotations"] = sorted(set(merged[field + "_static_annotations"]) | {str(item) for item in value})
    by_pc = defaultdict(list)
    for key, block in grouped.items():
        by_pc[number(block["pc"])].append(key)
    edges, entries = [], {group_ids[str(n)] for n in capture.get("entries", []) if str(n) in group_ids}
    unresolved = 0
    for edge in capture["edges"]:
        if str(edge["from"]) not in group_ids:
            continue
        source = group_ids[str(edge["from"])]
        if "to" in edge:
            if str(edge["to"]) not in group_ids:
                unresolved += number(edge["count"])
                continue
            target = group_ids[str(edge["to"])]
        else:
            candidates = by_pc[number(edge["to_pc"])]
            if len(candidates) != 1:
                unresolved += number(edge["count"])
                continue
            target = candidates[0]
        kind = edge.get("kind", "branch")
        if kind in ("call", "exception", "discontinuity"):
            entries.add(target)
        edges.append(dict(edge, **{"from": source, "to": target}))
    return grouped, edges, entries, unresolved


def code_signature(block):
    return (number(block["pc"]), tuple((number(ins["pc"]), ("opcode", number(ins["opcode"])) if "opcode" in ins else ("text", ins["text"])) if isinstance(ins, dict) else ins for ins in block.get("disassembly", [])))


def combine_captures(sample_capture, count_capture):
    """Use uncounted sample CPU weights and a separate counted replay's CFG."""
    # JSON roundtrip makes ownership explicit: inputs remain untouched.
    combined = json.loads(json.dumps(count_capture))
    sampled_blocks = {str(block["id"]): dict(block) for block in sample_capture["blocks"]}
    summary = map_samples(sample_capture, sampled_blocks) if isinstance(sample_capture.get("samples"), list) else sample_capture.get("samples", {})
    by_code = defaultdict(lambda: {"cpu_samples": 0.0, "cpu_sample_categories": defaultdict(float), "guest_pc_samples": defaultdict(float), "native_leaf_samples": defaultdict(float)})
    for block in sampled_blocks.values():
        if not block.get("disassembly"):
            continue
        row = by_code[code_signature(block)]
        row["cpu_samples"] += float(block.get("cpu_samples", 0))
        for field in ("cpu_sample_categories", "guest_pc_samples", "native_leaf_samples"):
            for key, weight in block.get(field, {}).items():
                row[field][key] += weight
    matched = set()
    executed_sources = {str(edge["from"]) for edge in combined.get("edges", []) if number(edge["count"]) > 0}
    for block in combined["blocks"]:
        signature = code_signature(block)
        for field in ("cpu_samples", "cpu_sample_categories", "guest_pc_samples", "native_leaf_samples"):
            block[field] = 0 if field == "cpu_samples" else {}
        if signature in by_code and signature not in matched and block.get("disassembly") and (number(block.get("executions", 0)) > 0 or str(block["id"]) in executed_sources):
            for field, value in by_code[signature].items():
                block[field] = dict(value) if isinstance(value, dict) else value
            matched.add(signature)
    combined["samples"] = summary
    sample_metadata, count_metadata = sample_capture.get("metadata", {}), count_capture.get("metadata", {})
    combined["metadata"]["sample_capture_metadata"] = sample_metadata
    combined["metadata"]["count_capture_metadata"] = dict(count_metadata)
    combined["metadata"]["cpu_provenance"] = "CPU samples from a separate sample-only replay; CFG and execution counts from a counted replay; joined by guest PC and identical captured instruction addresses/opcodes"
    combined["metadata"]["capture_boundary_match"] = all(sample_metadata.get(key) == count_metadata.get(key) for key in ("start_cycle", "end_cycle"))
    combined["metadata"]["unmatched_sample_capture_cpu_samples"] = sum(row["cpu_samples"] for signature, row in by_code.items() if signature not in matched)
    combined["metadata"]["thread_cpu_ns"] = sample_metadata.get("thread_cpu_ns", 0)
    combined["metadata"]["process_cpu_ns"] = sample_metadata.get("process_cpu_ns", 0)
    combined["metadata"]["sample_period_ns"] = sample_metadata.get("sample_period_ns")
    return combined


def analyze(capture):
    blocks = {str(b["id"]): dict(b) for b in capture["blocks"]}
    if len(blocks) != len(capture["blocks"]):
        raise ValueError("block IDs must be unique (retain JIT generation identity)")
    sample_summary = (map_samples(capture, blocks) if isinstance(capture.get("samples"), list) else capture.get("samples", {}))
    unresolved_edges = 0
    raw_edges = capture["edges"]
    merged_entries = capture.get("entries", [])
    if any("to_pc" in edge for edge in raw_edges):
        if capture.get("metadata", {}).get("mode") in ("both", "counts"):
            active_ids = {str(edge["from"]) for edge in raw_edges if number(edge["count"]) > 0}
            blocks = {key: block for key, block in blocks.items() if key in active_ids or number(block.get("executions", 0)) > 0 or float(block.get("cpu_samples", 0)) > 0}
        blocks, raw_edges, merged_entries, unresolved_edges = merge_guest_generations(capture, blocks)
    edges = defaultdict(int)
    estimated_edges = defaultdict(int)
    excluded_edges = 0
    for edge in raw_edges:
        source, target, count = str(edge["from"]), str(edge["to"]), number(edge["count"])
        if count < 0:
            raise ValueError("negative edge count")
        if source not in blocks or target not in blocks:
            raise ValueError("edge refers to missing block")
        if edge.get("kind", "branch") in ("call", "return", "exception", "scheduler", "discontinuity"):
            excluded_edges += count
            continue
        if count:
            if edge.get("kind") == "call_continuation_estimate":
                estimated_edges[source, target] += count
                edges[source, target] += 0
            else:
                edges[source, target] += count
    nodes = set(blocks)
    entries = {str(n) for n in merged_entries}
    if not entries <= nodes:
        raise ValueError("entry refers to missing block")
    loops = find_loops(nodes, edges, entries)
    for loop in loops:
        if loop["kind"] != "natural":
            loop["header"] = min(loop["members"], key=lambda node: (number(blocks[node]["pc"]), node))
    weights = {n: float(b.get("cpu_samples", 0)) for n, b in blocks.items()}
    if any(weight < 0 for weight in weights.values()):
        raise ValueError("negative CPU sample weight")
    mapped = sum(weights.values())
    total = float(sample_summary.get("total", mapped))
    if total < mapped:
        raise ValueError("total CPU samples smaller than mapped sample weights")
    owned = defaultdict(set)
    ambiguous, outside = set(), set()
    for node in nodes:
        candidates = [i for i, loop in enumerate(loops) if node in loop["members"]]
        innermost = [i for i in candidates if not any(loops[j]["members"] < loops[i]["members"] for j in candidates)]
        if len(innermost) == 1:
            owned[innermost[0]].add(node)
        elif innermost:
            ambiguous.add(node)
        else:
            outside.add(node)
    period_ns = sample_summary.get("cpu_period_ns")
    thread_cpu_ns = capture.get("metadata", {}).get("thread_cpu_ns", 0)
    process_cpu_ns = capture.get("metadata", {}).get("process_cpu_ns", 0)
    rows = []
    for i, loop in enumerate(loops):
        members, exclusive = loop["members"], owned[i]
        inclusive_weight, exclusive_weight = sum(weights[n] for n in members), sum(weights[n] for n in exclusive)
        calls = aggregate_annotations(blocks, exclusive, "fallback")
        synchronizations = aggregate_annotations(blocks, exclusive, "sync")
        sample_categories = defaultdict(float)
        for key in exclusive:
            for category, weight in blocks[key].get("cpu_sample_categories", {}).items():
                sample_categories[category] += weight
        native_leaf_samples = defaultdict(float)
        guest_pc_samples = defaultdict(float)
        for key in exclusive:
            for pc, weight in blocks[key].get("guest_pc_samples", {}).items():
                guest_pc_samples[f"0x{number(pc):08x}"] += weight
        for key in exclusive:
            for symbol, weight in blocks[key].get("native_leaf_samples", {}).items():
                native_leaf_samples[symbol] += weight
        parent_options = [j for j, other in enumerate(loops) if members < other["members"]]
        parent = min(parent_options, key=lambda j: len(loops[j]["members"])) if parent_options else None
        header = blocks[loop["header"]]
        row = {
            "id": i, "kind": loop["kind"], "header_id": loop["header"], "header_pc": f'0x{number(header["pc"]):08x}',
            "parent_id": parent, "block_ids": sorted(members), "exclusive_block_ids": sorted(exclusive),
            "inclusive_cpu_samples": inclusive_weight, "exclusive_cpu_samples": exclusive_weight,
            "exclusive_cpu_percent": 100 * exclusive_weight / total if total else 0,
            "exclusive_cpu_ms_estimate": (exclusive_weight / total * thread_cpu_ns / 1e6 if total and thread_cpu_ns else exclusive_weight * period_ns / 1e6 if period_ns else None),
            "exclusive_process_cpu_percent_estimate": (100 * exclusive_weight / total * thread_cpu_ns / process_cpu_ns if total and thread_cpu_ns and process_cpu_ns else None),
            "cpu_ms_method": "thread CPU clock scaled by sample share" if thread_cpu_ns else "sample count times nominal period" if period_ns else "unknown",
            "header_role": "natural_loop_header" if loop["kind"] == "natural" else "arbitrary_cycle_representative" if loop["kind"] == "two_block_cycle" else "arbitrary_scc_representative",
            "header_executions": header.get("executions") if loop["kind"] == "natural" else None,
            "representative_block_executions": header.get("executions"),
            "backedge_traversals": sum(edges[e] for e in loop["backedges"]),
            "backedge_count_complete": not any(estimated_edges[e] for e in loop["backedges"]),
            "backedge_continuation_upper_bound_estimate": sum(estimated_edges[e] for e in loop["backedges"]),
            "cycle_edge_traversals": [{"from": source, "to": target, "count": edges[source, target], "call_continuation_upper_bound_estimate": estimated_edges[source, target]} for source, target in loop.get("cycle_edges", [])],
            "completed_two_edge_cycles_upper_bound_estimate": min((edges[e] + estimated_edges[e] for e in loop.get("cycle_edges", [])), default=None),
            "backedges": [{"from": source, "to": target, "count": edges[source, target], "call_continuation_upper_bound_estimate": estimated_edges[source, target]} for source, target in loop["backedges"]],
            "fallback": calls, "sync": synchronizations, "exclusive_cpu_sample_categories": dict(sample_categories), "exclusive_native_leaf_samples": dict(native_leaf_samples), "exclusive_guest_pc_samples": dict(guest_pc_samples),
            "blocks": [blocks[n] for n in sorted(members)],
        }
        rows.append(row)
    rows.sort(key=lambda row: (-row["exclusive_cpu_samples"], number(row["header_pc"])))
    natural_rank = loop_rank = 0
    for rank, row in enumerate(rows, 1):
        row["rank"] = rank
        if row["kind"] in ("natural", "two_block_cycle"):
            loop_rank += 1
            row["loop_rank"] = loop_rank
        if row["kind"] == "natural":
            natural_rank += 1
            row["natural_loop_rank"] = natural_rank
    return {
        "metadata": capture.get("metadata", {}), "samples": sample_summary, "loops": rows,
        "accounting": {"total_cpu_samples": total, "mapped_cpu_samples": mapped,
                       "exclusive_loop_samples": sum(row["exclusive_cpu_samples"] for row in rows),
                       "two_block_cycle_exclusive_samples": sum(row["exclusive_cpu_samples"] for row in rows if row["kind"] == "two_block_cycle"),
                       "natural_loop_exclusive_samples": sum(row["exclusive_cpu_samples"] for row in rows if row["kind"] == "natural"),
                       "irreducible_residual_exclusive_samples": sum(row["exclusive_cpu_samples"] for row in rows if row["kind"] == "irreducible_scc"),
                       "unknown_entry_cyclic_residual_samples": sum(row["exclusive_cpu_samples"] for row in rows if row["kind"] == "cyclic_scc_unknown_entry"),
                       "outside_loop_samples": sum(weights[n] for n in outside),
                       "ambiguous_overlap_samples": sum(weights[n] for n in ambiguous),
                       "unmapped_cpu_samples": total - mapped, "excluded_boundary_edge_executions": excluded_edges, "unresolved_target_edge_executions": unresolved_edges,
                       "mapped_coverage_percent": 100 * mapped / total if total else 0,
                       "mapped_to_guest_instruction_samples": sum(sum(block.get("guest_pc_samples", {}).values()) for block in blocks.values()), "call_continuation_upper_bound_estimate": sum(estimated_edges.values())},
        "limitations": [
            "CPU contributions are statistical sample estimates; execution counts are separate evidence.",
            "Native return-frame attribution infers the calling guest block; unresolved/unwind-truncated samples remain unassigned. Runtime-call/register spans classify generated call/setup or state-access instructions, not all callee work.\n",
            "Loops use the observed intra-procedural CFG. Unobserved edges and unmarked call/return boundaries can affect loop identification.",
            "Two-block cycles come from reciprocal observed edges and may have multiple entries; each edge traversal is exact but their minimum is only an upper bound on uninterrupted completed cycles.\n",
            "Each mapped sample belongs to one innermost loop, the outside bucket, or the ambiguous overlap bucket. Inclusive rows overlap and must not be summed.",
            "Irreducible SCCs have no unique loop header or iteration count; source SCCs without a known entry are conservatively labeled cyclic_scc_unknown_entry, not declared irreducible. Their representative executions/backedge counts are not iteration estimates.",
            "Guest code generations are aggregated only when PC and captured instruction addresses/opcodes match. Ambiguous/missing guest target versions are excluded and counted as unresolved edges.\n",
            "Call-continuation edges are structural estimates (calls are an upper bound on returns), not measured return edges; backedge_count_complete is false when this affects an iteration count.\n",
            "Shared callees are separate regions: caller loop shares exclude callee work unless the native stack maps that sample directly back to the caller JIT block.\n",
            "Disassembler register-value annotations were captured during compilation and are not an execution trace; guest-code identity uses instruction addresses/opcodes, not those annotations.\n",
            "Fallback and synchronization counts do not establish CPU cost. Static sites or execution-weighted estimates must be labeled by the producer.",
        ],
    }


def markdown(report, limit=10):
    lines = ["# SH-4 loop and cyclic-region CPU profile", "",
             ("No runnable CPU samples were captured; a CPU-time ranking is unavailable." if not report["accounting"]["total_cpu_samples"] else "CPU-time contributions are statistical estimates."), "", "CPU percentages use all captured CPU samples as the denominator; rankings use exclusive innermost-loop ownership. irreducible_scc and cyclic_scc_unknown_entry rows are residual cyclic-region buckets, not natural loops; their addresses are arbitrary representatives and do not define iteration headers.", "", "| Rank | Header / representative | Kind | Exclusive CPU % (estimate) | Samples | Header executions | Backedge traversals |", "|---:|---|---|---:|---:|---:|---:|"]
    for row in report["loops"][:limit]:
        count = str(row["backedge_traversals"]) + (f' + continuation estimate <= {row["backedge_continuation_upper_bound_estimate"]}' if not row["backedge_count_complete"] else "")
        lines.append(f'| {row["rank"]} | {row["header_pc"]} | {row["kind"]} | {row["exclusive_cpu_percent"]:.3f} | {row["exclusive_cpu_samples"]:g} | {row["header_executions"] if row["header_executions"] is not None else ("n/a" if row["kind"] != "natural" else "unknown")} | {count if row["kind"] == "natural" else ("cycle edges: " + ", ".join(str(edge["count"]) for edge in row["cycle_edge_traversals"]) if row["kind"] == "two_block_cycle" else "n/a")} |')
    lines += ["", "## Capture provenance", "", "```json", json.dumps(report["metadata"], indent=2), "```", "", "## Accounting", "", "```json", json.dumps(report["accounting"], indent=2), "```", "", "CPU sample/mapping summary:", "", "```json", json.dumps(report["samples"], indent=2), "```", ""]
    for row in report["loops"][:limit]:
        lines += [f'## {row["rank"]}. {row["header_pc"]}', "", f'Parent loop ID: {row["parent_id"]}; inclusive samples: {row["inclusive_cpu_samples"]:g}; exclusive samples: {row["exclusive_cpu_samples"]:g}.', "", "Exclusive CPU sample categories (estimates): " + json.dumps(row["exclusive_cpu_sample_categories"], sort_keys=True), "", "Mapped native leaf CPU samples (caller attribution estimates): " + json.dumps(row["exclusive_native_leaf_samples"], sort_keys=True), "", "Guest instruction CPU sample estimates (remaining mapped samples are block-level only): " + json.dumps(row["exclusive_guest_pc_samples"], sort_keys=True), "", "Fallback counts/estimates: " + json.dumps(row["fallback"], sort_keys=True), "", "State synchronization counts/estimates: " + json.dumps(row["sync"], sort_keys=True), "", "```text"]
        for block in row["blocks"]:
            lines.append(f'block {block["id"]} SH4 0x{number(block["pc"]):08x} executions={block.get("executions", "unknown")} samples={block.get("cpu_samples", 0)}')
            disassembly = block.get("disassembly", [])
            if not disassembly:
                lines.append("  disassembly unavailable")
            for instruction in disassembly:
                lines.append("  " + (instruction if isinstance(instruction, str) else f'0x{number(instruction["pc"]):08x} {number(instruction.get("opcode", 0)):04x}  {instruction["text"]}'))
        lines += ["```", ""]
    lines += ["## Limitations", ""] + ["- " + value for value in report["limitations"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--counts", type=Path, help="separate deterministic counted replay; input capture supplies sample-only CPU weights")
    parser.add_argument("--json", type=Path, required=True, help="complete loop report")
    parser.add_argument("--markdown", type=Path, help="top-loop report with complete captured disassembly")
    parser.add_argument("--top", type=int, default=10)
    args = parser.parse_args()
    capture = json.loads(args.capture.read_text())
    if args.counts:
        capture = combine_captures(capture, json.loads(args.counts.read_text()))
        capture["metadata"]["sample_capture_path"] = str(args.capture.resolve())
        capture["metadata"]["count_capture_path"] = str(args.counts.resolve())
    report = analyze(capture)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    if args.markdown:
        args.markdown.write_text(markdown(report, args.top))
    print(json.dumps(report["accounting"], indent=2))


if __name__ == "__main__":
    main()
