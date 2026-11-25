// Opt-in diagnostic capture. Guest clocks and scheduler deadlines are never changed.
#include "sh4_loop_profiler.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/dyna/ngen.h"
#include "cfg/option.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <time.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_ARM64 && defined(TEST_AUTOMATION)
extern u64 arm64_guarded_poll_trace_entries();
#endif

namespace {
uint64_t trace_entries() {
#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_ARM64 && defined(TEST_AUTOMATION)
 return arm64_guarded_poll_trace_entries();
#else
 return 0;
#endif
}
uint64_t now_ns() {
 return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t cpu_ns(bool process=false) {
#if defined(__APPLE__) && defined(__aarch64__)
 timespec ts{}; clock_gettime(process?CLOCK_PROCESS_CPUTIME_ID:CLOCK_THREAD_CPUTIME_ID, &ts);
 return uint64_t(ts.tv_sec)*1000000000 + ts.tv_nsec;
#else
 return 0;
#endif
}
std::string quote(const std::string& s) {
 std::ostringstream o; o << '"';
 for (unsigned char c : s) { if(c=='"'||c=='\\') o << '\\' << c; else if(c<32) o << ' '; else o << c; }
 o << '"'; return o.str();
}
struct Instruction { uint32_t pc, opcode; std::string text; };
struct Block {
 Sh4LoopCounters counters, begin, end;
 uint64_t id=0, rx=0, size=0, created=0, retired=0;
 uint32_t pc=0, physical=0, branch=~0u, next=~0u, type=0;
 std::vector<Instruction> disassembly;
 std::vector<Sh4ProfileSpan> spans;
 std::vector<std::pair<uint32_t,uint64_t>> dynamic;
};
struct Sample { uint64_t time; std::vector<uint64_t> pcs; };
struct State {
 bool configured=false, counts=false, sampling=false, started=false, ended=false, finished=false;
 std::string path, mode;
 uint64_t start=0,end=~uint64_t(0), start_cycle=0,end_cycle=0,start_ns=0,end_ns=0,start_cpu=0,end_cpu=0,start_process=0,end_process=0;
 uint64_t start_trace_entries=0,end_trace_entries=0;
 std::atomic<bool> active{false}, stop{false};
 std::vector<std::unique_ptr<Block>> blocks;
 std::unordered_map<RuntimeBlockInfo*,Block*> live;
 std::vector<Sample> samples;
 std::thread sampler;
 uint64_t dropped=0, errors=0, attempts=0, missed_deadlines=0;
#if defined(__APPLE__) && defined(__aarch64__)
 mach_port_t target=MACH_PORT_NULL;
 uintptr_t stack_low=0,stack_high=0;
#endif
};
// Kept alive through emulator teardown / static destructor order.
State& state() { static State* s = new State; return *s; }
void configure() {
 auto& s=state(); if(s.configured) return; s.configured=true;
 const char* path=std::getenv("HOLLYCAST_SH4_PROFILE");
 if(!path || !*path) return;
 s.path=path;
 const char* mode=std::getenv("HOLLYCAST_SH4_PROFILE_MODE"); s.mode=mode?mode:"both";
 s.counts=s.mode=="both"||s.mode=="counts"; s.sampling=s.mode=="both"||s.mode=="sample";
 if(auto v=std::getenv("HOLLYCAST_SH4_PROFILE_START")) s.start=std::strtoull(v,nullptr,10);
 if(auto v=std::getenv("HOLLYCAST_SH4_PROFILE_END")) s.end=std::strtoull(v,nullptr,10);
#if defined(__APPLE__) && defined(__aarch64__)
 if(s.sampling) {
  s.target=mach_thread_self();
  s.stack_high=reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
  s.stack_low=s.stack_high-pthread_get_stacksize_np(pthread_self());
  s.samples.reserve(30000);
  s.sampler=std::thread([&s] {
   // 997 Hz avoids phase-locking to a 30/60 Hz guest cadence.
   auto deadline=std::chrono::steady_clock::now();
   while(!s.stop.load(std::memory_order_relaxed)) {
    deadline+=std::chrono::nanoseconds(1003009);
    const auto now=std::chrono::steady_clock::now();
    if(deadline<now) {++s.missed_deadlines; deadline=now+std::chrono::nanoseconds(1003009);}
    std::this_thread::sleep_until(deadline);
    if(!s.active.load(std::memory_order_acquire)) continue;
    ++s.attempts;
    thread_basic_info_data_t info{}; mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
    if(thread_info(s.target,THREAD_BASIC_INFO,reinterpret_cast<thread_info_t>(&info),&n)!=KERN_SUCCESS) {++s.errors; continue;}
    // Uniform wall-clock observations of RUNNING state estimate CPU residence;
    // waiting/pacing time is deliberately outside the CPU sample denominator.
    if(info.run_state!=TH_STATE_RUNNING) continue;
    if(thread_suspend(s.target)!=KERN_SUCCESS) {++s.errors; continue;}
    arm_thread_state64_t regs{}; n=ARM_THREAD_STATE64_COUNT;
    auto result=thread_get_state(s.target,ARM_THREAD_STATE64,reinterpret_cast<thread_state_t>(&regs),&n);
    const auto sample_time=now_ns();
    uint64_t pcs[26]; unsigned depth_count=0;
    if(result==KERN_SUCCESS) {
     pcs[depth_count++]=arm_thread_state64_get_pc(regs);
     pcs[depth_count++]=arm_thread_state64_get_lr(regs);
     uintptr_t fp=arm_thread_state64_get_fp(regs);
     for(unsigned depth=0;depth<24 && fp>=s.stack_low+4096 && fp+16<=s.stack_high && !(fp&15);++depth) {
      uint64_t words[2]; mach_vm_size_t read=0;
      if(mach_vm_read_overwrite(mach_task_self(),fp,16,reinterpret_cast<mach_vm_address_t>(words),&read)!=KERN_SUCCESS||read!=16) break;
      pcs[depth_count++]=words[1]&0x0000ffffffffffffULL; // strip return-address PAC
      if(words[0]<=fp) break;
      fp=words[0];
     }
    }
    // Always resume before allocations, symbol lookup, or output.
    thread_resume(s.target);
    if(result!=KERN_SUCCESS) {++s.errors; continue;}
    if(s.samples.size()<1000000) s.samples.push_back({sample_time,std::vector<uint64_t>(pcs,pcs+depth_count)}); else ++s.dropped;
   }
  });
 }
#endif
}
Block* get(RuntimeBlockInfo* block) {
 configure(); auto& s=state(); if(s.path.empty()||s.mode=="off") return nullptr;
 if(block->loop_profile_data) return static_cast<Block*>(block->loop_profile_data);
 auto b=std::make_unique<Block>(); b->id=s.blocks.size()+1; b->pc=block->vaddr; b->physical=block->addr;
 b->dynamic.reserve(4); b->counters.owner=b.get();
 auto ptr=b.get(); block->loop_profile_data=ptr; s.blocks.push_back(std::move(b)); s.live.emplace(block,ptr); return ptr;
}
}

bool sh4_loop_metadata_enabled() {
 configure(); return !state().path.empty()&&state().mode!="off";
}
Sh4LoopCounters* sh4_loop_counters(RuntimeBlockInfo* block) {
 auto* b=get(block); return state().counts&&b?&b->counters:nullptr;
}
void sh4_loop_register(RuntimeBlockInfo* block) {
 auto* b=get(block); if(!b) return;
 b->created=now_ns(); b->rx=reinterpret_cast<uintptr_t>(CC_RW2RX(block->code)); b->size=block->host_code_size;
 b->branch=block->BranchBlock; b->next=block->NextBlock; b->type=block->BlockType; b->spans=block->profile_spans;
 // Static calls do not populate NextBlock in this decoder. This is only a
 // structural return-continuation estimate; never change the guest block.
 if(b->type==BET_StaticCall && b->next==~0u) b->next=block->vaddr+block->sh4_code_size;
 for(const auto& instruction:block->profile_guest_opcodes) {
  uint16_t op=instruction.second; char text[512]{};
  OpDesc[op]->Disassemble(text,instruction.first,op);
  b->disassembly.push_back({instruction.first,op,text});
 }
}
void sh4_loop_retire(RuntimeBlockInfo* block) {
 auto& s=state(); auto it=s.live.find(block); if(it==s.live.end()) return;
 it->second->retired=now_ns(); s.live.erase(it);
}
void sh4_loop_dynamic_edge(Sh4LoopCounters* counters,uint32_t target) {
 auto& s=state(); if(!s.started||s.ended||!counters) return;
 // Ownership and counter addresses stay valid across code-cache resets.
 auto* b=static_cast<Block*>(counters->owner);
 for(auto& edge:b->dynamic) if(edge.first==target) {++edge.second;return;}
 b->dynamic.emplace_back(target,1);
}
void sh4_loop_tick(uint64_t cycle) {
 auto& s=state(); if(!s.configured||s.path.empty()||s.finished||s.ended) return;
 if(!s.started && cycle>=s.start) {
  s.started=true; s.start_cycle=cycle; s.start_ns=now_ns(); s.start_cpu=cpu_ns(); s.start_process=cpu_ns(true);
  s.start_trace_entries=trace_entries();
  for(auto& b:s.blocks) b->begin=b->counters;
  s.active.store(true,std::memory_order_release);
 }
 if(s.started && cycle>=s.end) {
  s.active.store(false,std::memory_order_release); s.end_cpu=cpu_ns(); s.end_process=cpu_ns(true); s.end_ns=now_ns(); s.end_cycle=cycle; s.ended=true;
  s.end_trace_entries=trace_entries();
  for(auto& b:s.blocks) b->end=b->counters;
 }
}
void sh4_loop_finish() {
 auto& s=state(); if(s.finished||s.path.empty()) return;
 s.finished=true; s.active.store(false); s.stop.store(true);
 if(s.sampler.joinable()) s.sampler.join();
#if defined(__APPLE__) && defined(__aarch64__)
 if(s.target!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),s.target);
#endif
 std::ofstream out(s.path);
 out << "{\"metadata\":{\"schema_version\":1,\"mode\":" << quote(s.mode)
 << ",\"jit_backend\":\"ARM64\",\"complete\":" << (s.ended?"true":"false")
 << ",\"start_cycle\":" << s.start_cycle << ",\"end_cycle\":" << s.end_cycle
 << ",\"start_ns\":" << s.start_ns << ",\"end_ns\":" << s.end_ns
 << ",\"thread_cpu_ns\":" << (s.ended?s.end_cpu-s.start_cpu:0)
 << ",\"process_cpu_ns\":" << (s.ended?s.end_process-s.start_process:0)
 << ",\"guarded_poll_trace_enabled\":" << (config::GuardedPollTrace?"true":"false")
 << ",\"guarded_poll_trace_entries\":" << (s.ended?s.end_trace_entries-s.start_trace_entries:0)
 << ",\"sample_period_ns\":1003009,\"sample_attempts\":" << s.attempts
 << ",\"sample_errors\":" << s.errors << ",\"dropped_samples\":" << s.dropped
 << ",\"missed_sample_deadlines\":" << s.missed_deadlines
 << ",\"total_runnable_samples\":" << std::count_if(s.samples.begin(),s.samples.end(),[&](const Sample& sample){return sample.time>=s.start_ns&&sample.time<s.end_ns;})
 << ",\"cpu_method\":\"997Hz wall observations conditional on Mach RUNNING (includes runnable); CPU time and attribution are estimates, emulation thread only\"},\"blocks\":[";
 bool comma=false;
 for(auto& b:s.blocks) {
  if(comma) out << ','; comma=true;
  out << "{\"id\":" << b->id << ",\"pc\":" << b->pc << ",\"physical\":" << b->physical
  << ",\"rx_begin\":" << b->rx << ",\"rx_end\":" << b->rx+b->size
  << ",\"created_ns\":" << b->created << ",\"retired_ns\":" << (b->retired?b->retired:s.end_ns)
  << ",\"executions\":" << (b->end.executions-b->begin.executions) << ",\"disassembly\":[";
  bool sep=false; for(auto& i:b->disassembly) {if(sep)out<<',';sep=true;out<<"{\"pc\":"<<i.pc<<",\"opcode\":"<<i.opcode<<",\"text\":"<<quote(i.text)<<'}';}
  const auto executions=b->end.executions-b->begin.executions;
  auto sites=[&](Sh4ProfileSpanKind kind) {return uint64_t(std::count_if(b->spans.begin(),b->spans.end(),[&](const Sh4ProfileSpan& p){return p.kind==kind;}))*executions;};
  out << "],\"fallback\":{\"interpreter_sites_x_block_entries_upper_bound\":" << sites(Sh4ProfileSpanKind::InterpreterFallback)
      << "},\"sync\":{\"allocator_loads_x_block_entries_upper_bound\":" << sites(Sh4ProfileSpanKind::RegisterLoad)
      << ",\"allocator_stores_x_block_entries_upper_bound\":" << sites(Sh4ProfileSpanKind::RegisterStore)
      << "},\"profile_spans\":["; sep=false;
  for(auto& p:b->spans) {
   if(sep)out<<',';sep=true;
   out<<"{\"host_offset\":"<<p.host_offset<<",\"host_size\":"<<p.host_size<<",\"guest_pc\":"<<p.guest_pc<<",\"kind\":"<<unsigned(p.kind)<<",\"target\":"<<p.target;
#if defined(__unix__) || defined(__APPLE__)
   Dl_info info{}; if(p.target&&dladdr(reinterpret_cast<void*>(p.target),&info)&&info.dli_sname) out<<",\"target_symbol\":"<<quote(info.dli_sname);
#endif
   out<<'}';
  }
  out << "]}";
 }
 out << "],\"edges\":["; comma=false;
 auto edge=[&](uint64_t from,uint32_t to,uint64_t count,const char* kind) { if(!count||to==~0u)return; if(comma)out<<',';comma=true;out<<"{\"from\":"<<from<<",\"to_pc\":"<<to<<",\"count\":"<<count<<",\"kind\":"<<quote(kind)<<'}'; };
 for(auto& b:s.blocks) {
  const char* kind=(b->type==BET_StaticCall||b->type==BET_DynamicCall)?"call":b->type==BET_DynamicRet?"return":(b->type==BET_StaticIntr||b->type==BET_DynamicIntr)?"exception":"branch";
  edge(b->id,b->branch,b->end.branch-b->begin.branch,kind);edge(b->id,b->next,b->end.next-b->begin.next,"branch");
  uint64_t calls=b->end.branch-b->begin.branch;
  for(auto& e:b->dynamic) {edge(b->id,e.first,e.second,b->type==BET_StaticIntr&&e.first==b->next?"branch":kind);calls+=e.second;}
  if(b->type==BET_StaticCall||b->type==BET_DynamicCall) edge(b->id,b->next,calls,"call_continuation_estimate");
 }
 out << "],\"samples\":[";comma=false;
 for(auto& sample:s.samples) {
  if(sample.time<s.start_ns||sample.time>=s.end_ns)continue;
  if(comma)out<<',';comma=true;out<<"{\"host_ns\":"<<sample.time<<",\"pcs\":[";
  bool sep=false;for(auto pc:sample.pcs){if(sep)out<<',';sep=true;out<<pc;}out<<']';
#if defined(__unix__) || defined(__APPLE__)
  Dl_info info{}; if(!sample.pcs.empty()&&dladdr(reinterpret_cast<void*>(sample.pcs.front()),&info)&&info.dli_sname) out<<",\"leaf_symbol\":"<<quote(info.dli_sname);
#endif
  out<<'}';
 }
 out << "]}\n";
}
