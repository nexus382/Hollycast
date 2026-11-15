#pragma once
#include <cstdint>
struct RuntimeBlockInfo;
struct Sh4LoopCounters {
	uint64_t executions = 0, branch = 0, next = 0;
	void* owner = nullptr;
};
#ifdef SH4_LOOP_PROFILER
bool sh4_loop_metadata_enabled();
Sh4LoopCounters* sh4_loop_counters(RuntimeBlockInfo* block);
void sh4_loop_register(RuntimeBlockInfo* block);
void sh4_loop_retire(RuntimeBlockInfo* block);
void sh4_loop_dynamic_edge(Sh4LoopCounters* counters, uint32_t target);
void sh4_loop_tick(uint64_t cycle);
void sh4_loop_finish();
#else
inline void sh4_loop_register(RuntimeBlockInfo*) {}
inline void sh4_loop_retire(RuntimeBlockInfo*) {}
inline void sh4_loop_tick(uint64_t) {}
inline void sh4_loop_finish() {}
#endif
