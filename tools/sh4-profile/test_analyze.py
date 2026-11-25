import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("analyze", Path(__file__).with_name("analyze.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def block(key, samples=0):
    return {"id": key, "pc": hex(0x8c000000 + key * 2), "cpu_samples": samples, "executions": key * 100}


def edge(source, target, count=1, kind="branch"):
    return {"from": source, "to": target, "count": count, "kind": kind}


class AnalysisTests(unittest.TestCase):
    def test_nested_loop_samples_are_owned_once(self):
        # 0 -> outer header 1 -> inner header 2 -> latch 3 -> outer latch 4.
        capture = {"blocks": [block(0, 1), block(1, 10), block(2, 30), block(3, 40), block(4, 20)],
                   "edges": [edge(0, 1), edge(1, 2), edge(2, 3), edge(3, 2, 900), edge(3, 4), edge(4, 1, 90)],
                   "entries": [0], "samples": {"total": 111, "cpu_period_ns": 1000000}}
        report = module.analyze(capture)
        inner, outer = report["loops"]
        self.assertEqual(inner["header_id"], "2")
        self.assertEqual(inner["exclusive_cpu_samples"], 70)
        self.assertEqual(outer["exclusive_cpu_samples"], 30)
        self.assertEqual(outer["inclusive_cpu_samples"], 100)
        self.assertEqual(inner["parent_id"], outer["id"])
        self.assertEqual(inner["backedge_traversals"], 900)
        self.assertEqual(report["accounting"]["unmapped_cpu_samples"], 10)
        self.assertEqual(report["accounting"]["outside_loop_samples"], 1)
        self.assertEqual(report["accounting"]["exclusive_loop_samples"], 100)

    def test_call_and_return_do_not_invent_loops(self):
        capture = {"blocks": [block(0, 1), block(1, 2)], "edges": [edge(0, 1, kind="call"), edge(1, 0, kind="return")], "entries": [0]}
        self.assertEqual(module.analyze(capture)["loops"], [])

    def test_irreducible_cycle_is_reported(self):
        capture = {"blocks": [block(i, i) for i in range(4)], "edges": [edge(0, 1), edge(0, 2), edge(0, 3), edge(1, 2), edge(2, 3), edge(3, 1)], "entries": [0]}
        loops = module.analyze(capture)["loops"]
        self.assertEqual(len(loops), 1)
        self.assertEqual(loops[0]["kind"], "irreducible_scc")
        self.assertEqual(loops[0]["exclusive_cpu_samples"], 6)

    def test_sample_mapping_respects_lifetimes_and_return_addresses(self):
        blocks = [dict(block(i), rx_begin="0x1000", rx_end="0x1010", created_ns=i * 100,
                       retired_ns=(i + 1) * 100, profile_spans=[{"host_offset": 0, "host_size": 16, "guest_pc": "0x8c000000", "kind": 0},
                                                                             {"host_offset": 12, "host_size": 4, "kind": 1, "target": "0x2000", "target_symbol": "slow_read"}]) for i in range(2)]
        capture = {"blocks": blocks, "edges": [edge(0, 0), edge(1, 1)], "entries": [0, 1],
                   "samples": [{"host_ns": 10, "pcs": ["0x1004"]}, {"host_ns": 110, "pcs": ["0x2000", "0x1010"]},
                               {"host_ns": 200, "pcs": ["0x1004"]}, {"host_ns": 10, "pcs": ["0x1004"], "runnable": False}],
                   "metadata": {"sample_period_ns": 1000000}}
        report = module.analyze(capture)
        self.assertEqual(report["samples"]["mapped_direct_jit"], 1)
        self.assertEqual(report["samples"]["mapped_via_jit_caller"], 1)
        self.assertEqual(report["samples"]["unmapped"], 1)
        self.assertEqual(report["samples"]["excluded_non_runnable"], 1)
        categories = next(row for row in report["loops"] if row["header_id"] == "1")["exclusive_cpu_sample_categories"]
        self.assertEqual(categories["callee_via_jit_return/runtime_call/slow_read"], 1)

    def test_to_pc_edges_merge_only_identical_guest_code(self):
        first = dict(block(0, 3), id=1, executions=10, disassembly=[{"pc": "0x8c000000", "opcode": "0x0009", "text": "nop"}])
        second = dict(first, id=2, cpu_samples=4, executions=20, disassembly=[{"pc": "0x8c000000", "opcode": "0x0009", "text": "nop ; changed compilation-time register annotation"}])
        capture = {"blocks": [first, second], "edges": [{"from": 1, "to_pc": first["pc"], "count": 9}, {"from": 2, "to_pc": first["pc"], "count": 19}], "entries": [1], "samples": {"total": 10}}
        report = module.analyze(capture)
        loop = report["loops"][0]
        self.assertEqual(loop["exclusive_cpu_samples"], 7)
        self.assertEqual(loop["header_executions"], 30)
        self.assertEqual(loop["backedge_traversals"], 28)
        self.assertEqual(loop["blocks"][0]["generation_ids"], ["1", "2"])
        changed = dict(second, id=3, disassembly=[{"pc": "0x8c000000", "opcode": "0x001b", "text": "sleep"}])
        capture["blocks"].append(changed)
        capture["samples"]["total"] = 20
        report = module.analyze(capture)
        self.assertEqual(report["accounting"]["unresolved_target_edge_executions"], 28)

    def test_cycle_without_known_entry_is_not_declared_irreducible(self):
        capture = {"blocks": [block(0, 1), block(1, 2), block(2, 3)], "edges": [edge(0, 1), edge(1, 2), edge(2, 0)]}
        row = module.analyze(capture)["loops"][0]
        self.assertEqual(row["kind"], "cyclic_scc_unknown_entry")
        self.assertIsNone(row["header_executions"])
        self.assertEqual(row["header_role"], "arbitrary_scc_representative")

    def test_two_block_cycle_owns_samples_inside_irreducible_parent(self):
        capture = {"blocks": [block(i, i) for i in range(4)], "edges": [edge(0, 1), edge(0, 2), edge(1, 2, 100), edge(2, 1, 90), edge(2, 3), edge(3, 1)], "entries": [0]}
        report = module.analyze(capture)
        short = next(row for row in report["loops"] if row["kind"] == "two_block_cycle")
        residual = next(row for row in report["loops"] if row["kind"] == "irreducible_scc")
        self.assertEqual(short["exclusive_cpu_samples"], 3)
        self.assertEqual(short["completed_two_edge_cycles_upper_bound_estimate"], 90)
        self.assertEqual(residual["exclusive_cpu_samples"], 3)
        self.assertEqual(report["accounting"]["exclusive_loop_samples"], 6)

    def test_call_continuation_is_not_an_exact_backedge_count(self):
        capture = {"blocks": [block(i, i) for i in range(3)], "edges": [edge(0, 1, 50), edge(1, 0, 50, "call_continuation_estimate"), edge(1, 2, 50, "call"), edge(2, 0, 50, "return")], "entries": [0]}
        loop = module.analyze(capture)["loops"][0]
        self.assertEqual(loop["block_ids"], ["0", "1"])
        self.assertFalse(loop["backedge_count_complete"])
        self.assertEqual(loop["backedge_traversals"], 0)
        self.assertEqual(loop["backedge_continuation_upper_bound_estimate"], 50)

    def test_separate_replay_join_does_not_multiply_samples_across_generations(self):
        instruction = [{"pc": "0x8c000000", "opcode": "0x0009", "text": "nop"}]
        sample = {"blocks": [dict(block(0, 7), id=40, disassembly=instruction)], "edges": [], "samples": {"total": 10}, "metadata": {"mode": "sample", "thread_cpu_ns": 1000000000, "start_cycle": 10, "end_cycle": 20}}
        counted = {"blocks": [dict(block(0), id=1, disassembly=instruction, executions=10), dict(block(0), id=2, disassembly=instruction, executions=20)], "edges": [{"from": 1, "to_pc": "0x8c000000", "count": 9}, {"from": 2, "to_pc": "0x8c000000", "count": 19}], "entries": [1], "metadata": {"mode": "counts", "start_cycle": 10, "end_cycle": 20}}
        joined = module.combine_captures(sample, counted)
        report = module.analyze(joined)
        self.assertEqual(report["accounting"]["mapped_cpu_samples"], 7)
        self.assertEqual(report["loops"][0]["header_executions"], 30)
        self.assertEqual(report["loops"][0]["exclusive_cpu_ms_estimate"], 700)
        self.assertTrue(report["metadata"]["capture_boundary_match"])
        self.assertEqual(counted["blocks"][0]["cpu_samples"], 0)

    def test_reject_inconsistent_sample_totals(self):
        with self.assertRaises(ValueError):
            module.analyze({"blocks": [block(0, 2)], "edges": [], "samples": {"total": 1}})


if __name__ == "__main__":
    unittest.main()
