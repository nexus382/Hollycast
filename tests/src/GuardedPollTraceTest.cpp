// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/dyna/ngen.h"
#include "oslib/oslib.h"
#include "audio/audiostream.h"
#include "hw/pvr/Renderer_if.h"
#include "json.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <time.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if HOST_CPU == CPU_ARM64 && FEAT_SHREC == DYNAREC_JIT && defined(TEST_AUTOMATION)
extern u64 arm64_guarded_poll_trace_entries();

namespace {
constexpr u32 A = 0x8c05b3a4;
constexpr u32 B = 0x8c05b3d0;
constexpr u32 Literal = 0x8c05b3cc;
constexpr u32 Island = 0x8c05b68e;
constexpr u32 Poll = 0x8c020000;
constexpr u32 Stack = 0x8c040000;
constexpr u32 Vbr = 0x8c060000;

struct Architecture {
    std::array<u32, 16> r;
    std::array<u32, 8> bank;
    std::array<u32, 16> fr, xf;
    u32 pc, sr, ssr, spc, sgr, vbr, gbr, pr, fpul, fpscr, oldFpscr, dbr, expevt, intevt;
    u64 mac;
    int cycles;
    bool operator==(const Architecture& other) const {
        return r==other.r && bank==other.bank && fr==other.fr && xf==other.xf
            && pc==other.pc && sr==other.sr && ssr==other.ssr && spc==other.spc
            && sgr==other.sgr && vbr==other.vbr && gbr==other.gbr && pr==other.pr
            && fpul==other.fpul && fpscr==other.fpscr && oldFpscr==other.oldFpscr
            && dbr==other.dbr && expevt==other.expevt && intevt==other.intevt
            && mac==other.mac && cycles==other.cycles;
    }
};
Architecture architecture() {
    Architecture result{};
    std::copy_n(Sh4cntx.r,16,result.r.begin());
    std::copy_n(Sh4cntx.r_bank,8,result.bank.begin());
    std::memcpy(result.fr.data(),Sh4cntx.fr,sizeof(result.fr));
    std::memcpy(result.xf.data(),Sh4cntx.xf,sizeof(result.xf));
    result.pc=Sh4cntx.pc;result.sr=Sh4cntx.sr.getFull();result.ssr=Sh4cntx.ssr;
    result.spc=Sh4cntx.spc;result.sgr=Sh4cntx.sgr;result.vbr=Sh4cntx.vbr;
    result.gbr=Sh4cntx.gbr;result.pr=Sh4cntx.pr;result.fpul=Sh4cntx.fpul;
    result.mac=Sh4cntx.mac.full;result.cycles=Sh4cntx.cycle_counter;
    result.fpscr=Sh4cntx.fpscr.full;result.oldFpscr=Sh4cntx.old_fpscr.full;
    result.dbr=Sh4cntx.dbr;result.expevt=CCN_EXPEVT;result.intevt=CCN_INTEVT;
    return result;
}
struct Tick {
    u64 now;
    int elapsed,jitter;
    Architecture state;
    bool operator==(const Tick& other) const {
        return now==other.now && elapsed==other.elapsed && jitter==other.jitter && state==other.state;
    }
};
enum class Change { None, PollByte, StackFlag, LiteralPointer, StackPointer, Opcode, BranchIsland, Interrupt };
struct Scenario {
    u8 poll=0, comparison=1;
    u32 flag=0;
    u32 stack=Stack;
    u32 pollAddress=Poll;
    u16 firstOpcode=0xd309, flagOpcode=0x2338, branchDelay=0x0009;
    int initialCycles=SH4_TIMESLICE;
    bool mmu=false, twoHop=false, startAtB=false, benchmark=false;
    u16 islandDelay=0x0009;
    Change change=Change::None;
};
struct BenchmarkMeasurement {
    u64 guestCycles=0,callbacks=0,digest=14695981039346656037ULL,traceEntries=0;
    double threadCpuSeconds=0,wallSeconds=0;
};
u64 threadCpuNs() {
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec value{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value);
    return u64(value.tv_sec)*1000000000ULL+value.tv_nsec;
#else
    return 0;
#endif
}
u64 steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Result {
    Architecture final;
    std::vector<Tick> ticks;
    std::array<u8,32> stackBytes;
    u8 poll;
    u32 literal;
    u16 flagOpcode;
    u64 traceEntries;
    BenchmarkMeasurement measurement;
};

class GuardedPollTraceTest : public ::testing::Test {
public:
    static void SetUpTestSuite() { os_InstallFaultHandler(); }
    static void TearDownTestSuite() { os_UninstallFaultHandler(); }
protected:
    void SetUp() override {
        ASSERT_TRUE(addrspace::reserve());
        config::DynarecEnabled=true;
        emu.init();
        mem_map_default();
    }
    void TearDown() override {
        if(callback!=-1)sh4_sched_unregister(callback);
        callback=-1;
        Sh4cntx.CpuRunning=false;
        CCN_MMUCR.AT=0;mmu_set_state();
        emu.getSh4Executor()->Reset(true);
        config::GuardedPollTrace=true;
        setDefaultRoundingMode();
    }
    void write(u32 pc,std::initializer_list<u16> code) {
        for(u16 op:code){addrspace::write16(pc,op);pc+=2;}
    }
    static int tick(int,int elapsed,int jitter,void* arg) {
        auto& self=*static_cast<GuardedPollTraceTest*>(arg);
        if(self.current.benchmark)return self.benchmarkTick(elapsed,jitter);
        self.ticks.push_back({sh4_sched_now64(),elapsed,jitter,architecture()});
        const size_t index=self.ticks.size();
        if(index==3) {
            switch(self.current.change) {
            case Change::None:break;
            case Change::PollByte:addrspace::write8(self.current.pollAddress,2);break;
            case Change::StackFlag:addrspace::write32(self.current.stack+12,0);break;
            case Change::LiteralPointer:
                addrspace::write8(Poll+0x100,2);
                addrspace::write32(Literal,Poll+0x100);
                break;
            case Change::StackPointer:
                // A scheduler event may change architectural registers. The
                // optimized path must reload the changed R15 before resuming.
                Sh4cntx.r[15]=Stack+0x100;
                break;
            case Change::Opcode:
                // Replace TST with CLRT: changing the code must invalidate the
                // fused trace, not let it use the obsolete flag-test body.
                addrspace::write16(B+2,0x0008);
                break;
            case Change::BranchIsland:addrspace::write16(Island+2,0xe405);break;
            case Change::Interrupt:SetInterruptPend(sh4_IRL_9);break;
            }
        }
        if(index==4 && self.current.change==Change::Interrupt)ResetInterruptPend(sh4_IRL_9);
        // Let the already-entered generic block finish before changing data:
        // CheckBlock precedes scheduler callbacks in the reference JIT too.
        if(index==4 && (self.current.change==Change::Opcode || self.current.change==Change::BranchIsland))addrspace::write32(self.current.stack+12,0);
        if(index>=7){Sh4cntx.CpuRunning=false;return 0;}
        return 137;
    }
    int benchmarkTick(int elapsed,int jitter) {
        const u64 now=sh4_sched_now64();
        if(!benchmarkMeasuring && now>=benchmarkWarmGoal) {
            benchmarkMeasuring=true;benchmarkStartGuest=now;
            benchmarkStartCpu=threadCpuNs();benchmarkStartWall=steadyNs();
            benchmarkStartEntries=arm64_guarded_poll_trace_entries();
        }
        if(benchmarkMeasuring) {
            ++measurement.callbacks;
            auto mix=[&](u64 value) {for(unsigned i=0;i<8;i++){measurement.digest^=value&255;measurement.digest*=1099511628211ULL;value>>=8;}};
            mix(now-benchmarkStartGuest);mix(elapsed);mix(jitter);mix(Sh4cntx.pc);
            mix(Sh4cntx.cycle_counter);mix(Sh4cntx.sr.getFull());
            for(unsigned i=0;i<4;i++)mix(Sh4cntx.r[i]);
            if(now-benchmarkStartGuest>=SH4_MAIN_CLOCK*2ULL) {
                measurement.threadCpuSeconds=double(threadCpuNs()-benchmarkStartCpu)/1e9;
                measurement.wallSeconds=double(steadyNs()-benchmarkStartWall)/1e9;
                measurement.guestCycles=now-benchmarkStartGuest;
                measurement.traceEntries=arm64_guarded_poll_trace_entries()-benchmarkStartEntries;
                Sh4cntx.CpuRunning=false;
                return 0;
            }
        }
        return 8192;
    }
    Result run(bool enable,Scenario scenario) {
        SCOPED_TRACE(enable?"guarded trace":"generic JIT");
        if(callback!=-1){sh4_sched_unregister(callback);callback=-1;}
        Sh4cntx.CpuRunning=false;
        CCN_MMUCR.AT=0;mmu_set_state();
        config::GuardedPollTrace=enable;
        emu.dc_reset(true);
        if(scenario.mmu) {
            UTLB[0].Address.VPN=0x02000000>>10;
            UTLB[0].Data.V=1;UTLB[0].Data.PR=3;UTLB[0].Data.SZ0=1;
            UTLB[0].Data.PPN=0x0c010000>>10;UTLB_Sync(0);
            CCN_MMUCR.AT=1;mmu_set_state();
            EXPECT_TRUE(mmu_enabled());
            emu.getSh4Executor()->Reset(true);
        }
        // These are real SH4 instructions at the measured addresses. Exit
        // markers and callback-driven mutations are independent of the trace
        // implementation; the generic JIT is the reference for cycle timing.
        write(A,{scenario.firstOpcode,0x6130,0xe013,0x02fc,0x3122,0x8b0f});
        write(A+12,{0xe701,0xaffe,0x0009}); // marker 1; bra marker loop; nop
        addrspace::write32(Literal,scenario.pollAddress);
        write(B,{0x53f3,scenario.flagOpcode,0x8901,static_cast<u16>(scenario.twoHop?0xa15a:0xafe5),scenario.branchDelay});
        // Actual game code reaches A through this second branch island.
        write(Island,{0xae89,scenario.islandDelay});
        write(B+10,{0xe702,0xaffe,0x0009}); // marker 2; bra marker loop; nop
        write(Vbr+0x600,{0x7801,0xe000,0x1f03,0x002b,0x0009}); // IRQ: r8++; flag=0; rte; nop
        write(Vbr+0x100,{0xe709,0xaffe,0x0009}); // exception marker, not a silent hang
        for(unsigned i=0;i<32;i++)addrspace::write8(scenario.stack+i,static_cast<u8>(0x40+i));
        addrspace::write8(scenario.pollAddress,scenario.poll);
        addrspace::write8(scenario.stack+19,scenario.comparison);
        addrspace::write8(scenario.stack+18,scenario.comparison);
        addrspace::write32(scenario.stack+12,scenario.flag);
        for(unsigned i=0;i<32;i++)addrspace::write8(Stack+0x100+i,0);
        addrspace::write8(Stack+0x100+19,1);
        for(unsigned i=0;i<16;i++)Sh4cntx.r[i]=0x11110000u+i*0x101;
        for(unsigned i=0;i<8;i++)Sh4cntx.r_bank[i]=0x22220000u+i*0x101;
        Sh4cntx.r[7]=Sh4cntx.r[8]=0;Sh4cntx.r[15]=scenario.stack;
        Sh4cntx.pc=A;Sh4cntx.vbr=Vbr;Sh4cntx.dbr=0x12345678;
        Sh4cntx.sr.setFull(scenario.benchmark?0x400000f0:0x40000000);Sh4cntx.old_sr.status=Sh4cntx.sr.status;UpdateSR();
        Sh4cntx.cycle_counter=scenario.initialCycles;
        if(scenario.startAtB) {
            // Compile A without executing it, so an A-true comparison can be
            // reached from the guarded B entry instead of exiting generically
            // before B is ever compiled.
            EXPECT_NE(rdv_FailedToFindBlock(A),nullptr);
            Sh4cntx.pc=B;
        }
        current=scenario;ticks.clear();measurement={};benchmarkMeasuring=false;
        benchmarkWarmGoal=sh4_sched_now64()+SH4_MAIN_CLOCK/2;
        callback=sh4_sched_register(0,tick,this);sh4_sched_request(callback,scenario.benchmark?8192:137);
        const u64 before=arm64_guarded_poll_trace_entries();
        auto* cpu=emu.getSh4Executor();cpu->Start();
        if(scenario.benchmark) {
            // CpuRunning must be true while compiling so generic blocks emit
            // their normal link stubs. Untimed actual execution below resolves
            // those stubs; code and link state remain warm across the timing
            // transition in benchmarkTick (no reset or artificial resume).
            EXPECT_NE(rdv_FailedToFindBlock(A),nullptr);
            EXPECT_NE(rdv_FailedToFindBlock(B),nullptr);
            Sh4cntx.pc=A;
        }
        cpu->Run();
        Result result{architecture(),ticks,{},addrspace::read8(scenario.pollAddress),addrspace::read32(Literal),
            addrspace::read16(B+2),arm64_guarded_poll_trace_entries()-before,measurement};
        for(unsigned i=0;i<result.stackBytes.size();i++)result.stackBytes[i]=addrspace::read8(scenario.stack+i);
        sh4_sched_unregister(callback);callback=-1;
        if(scenario.benchmark) {
            EXPECT_GE(result.measurement.guestCycles,SH4_MAIN_CLOCK*2ULL);
            EXPECT_GT(result.measurement.callbacks,0u);
        } else {
            EXPECT_GE(result.ticks.size(),7u);
            EXPECT_LT(result.ticks.size(),10u);
        }
        switch(scenario.change) {
        case Change::None:break;
        case Change::PollByte:EXPECT_EQ(result.final.r[7],1u);EXPECT_EQ(result.poll,2u);break;
        case Change::StackFlag:case Change::StackPointer:EXPECT_EQ(result.final.r[7],2u);break;
        case Change::LiteralPointer:EXPECT_EQ(result.final.r[7],1u);EXPECT_EQ(result.literal,Poll+0x100);break;
        case Change::Opcode:EXPECT_EQ(result.final.r[7],0u);EXPECT_EQ(result.flagOpcode,0x0008);break;
        case Change::BranchIsland:EXPECT_EQ(result.final.r[7],2u);EXPECT_EQ(result.final.r[4],5u);break;
        case Change::Interrupt:
            EXPECT_GT(result.final.r[8],0u)<<"the guest IRQ handler must actually execute";
            EXPECT_EQ(result.final.r[7],2u);
            EXPECT_EQ(result.final.intevt,static_cast<u32>(Sh4Ex_ExtInterrupt9));
            EXPECT_EQ(result.final.ssr,0x40000000u);
            EXPECT_TRUE(result.final.spc==A || result.final.spc==B);
            EXPECT_EQ(result.final.sgr,scenario.stack);
            EXPECT_EQ(result.final.sr,0x40000001u); // returned, unbanked, final TST is true
            break;
        }
        return result;
    }
    Result equivalent(Scenario scenario,bool expectTrace) {
        const auto reference=run(false,scenario);
        const auto actual=run(true,scenario);
        EXPECT_EQ(reference.traceEntries,0u);
        if(expectTrace)EXPECT_GT(actual.traceEntries,0u);else EXPECT_EQ(actual.traceEntries,0u);
        EXPECT_EQ(actual.final,reference.final);
        EXPECT_EQ(actual.ticks,reference.ticks);
        EXPECT_EQ(actual.stackBytes,reference.stackBytes);
        EXPECT_EQ(actual.poll,reference.poll);
        EXPECT_EQ(actual.literal,reference.literal);
        EXPECT_EQ(actual.flagOpcode,reference.flagOpcode);
        return actual;
    }
    BenchmarkMeasurement measurement;
    bool benchmarkMeasuring=false;
    u64 benchmarkWarmGoal=0,benchmarkStartGuest=0,benchmarkStartCpu=0,benchmarkStartWall=0,benchmarkStartEntries=0;
    int callback=-1;
    Scenario current;
    std::vector<Tick> ticks;
};

TEST_F(GuardedPollTraceTest, BothExitsAndSignExtendedByteComparisonsMatchGenericJit) {
    for(auto values:{std::array<u8,2>{0,1},{1,0},{0x80,0xff},{0xff,0x80},{0x80,0x7f},{0x7f,0x80}}) {
        Scenario scenario;scenario.poll=values[0];scenario.comparison=values[1];scenario.flag=0;
        const bool reachesB=static_cast<u32>(static_cast<s32>(static_cast<s8>(scenario.poll)))
            <static_cast<u32>(static_cast<s32>(static_cast<s8>(scenario.comparison)));
        auto checkGuestResult=[&](const Result& result) {
            EXPECT_EQ(result.final.r[7],reachesB?2u:1u);
            EXPECT_EQ(result.final.r[0],19u);
            EXPECT_EQ(result.final.r[1],static_cast<u32>(static_cast<s32>(static_cast<s8>(scenario.poll))));
            EXPECT_EQ(result.final.r[2],static_cast<u32>(static_cast<s32>(static_cast<s8>(scenario.comparison))));
            EXPECT_EQ(result.final.r[3],reachesB?0u:scenario.pollAddress);
            EXPECT_EQ(result.final.sr,0x40000001u);
        };
        checkGuestResult(equivalent(scenario,reachesB));
        // Every signed-byte pair must also execute A inside the fused trace.
        // Warm A by compilation only, then enter B with a nonzero stack flag.
        // False A comparisons loop until the scheduled flag change; true A
        // comparisons leave immediately through the trace's A side exit.
        scenario.startAtB=true;scenario.flag=1;
        scenario.change=reachesB?Change::StackFlag:Change::None;
        checkGuestResult(equivalent(scenario,true));
    }
}
TEST_F(GuardedPollTraceTest, ActualGameTwoHopLayoutUsesGuardedTrace) {
    Scenario scenario;scenario.twoHop=true;scenario.pollAddress=0x8c386928;scenario.stack=0x8c00ef90;
    scenario.flag=1;scenario.change=Change::StackFlag;equivalent(scenario,true);
}
TEST_F(GuardedPollTraceTest, SchedulerSeesCanonicalRegistersAtBothBoundaries) {
    for(bool chain:{false,true})for(int cycles:{-449,-448,-1,0,1,3,7,11,SH4_TIMESLICE})for(Change change:{Change::PollByte,Change::StackFlag,Change::StackPointer}) {
        Scenario scenario;scenario.flag=1;scenario.initialCycles=cycles;scenario.change=change;scenario.twoHop=chain;
        if(chain){scenario.pollAddress=0x8c386928;scenario.stack=0x8c00ef90;}
        equivalent(scenario,true);
    }
}
TEST_F(GuardedPollTraceTest, RealInterruptAndRtePreserveBankedRegistersAndResumePc) {
    Scenario scenario;scenario.flag=1;scenario.change=Change::Interrupt;equivalent(scenario,true);
}
TEST_F(GuardedPollTraceTest, SchedulerCodeAndLiteralWritesInvalidateCachedTrace) {
    for(Change change:{Change::LiteralPointer,Change::Opcode,Change::BranchIsland}) {
        Scenario scenario;scenario.flag=1;scenario.change=change;scenario.twoHop=change==Change::BranchIsland;
        if(scenario.twoHop){scenario.pollAddress=0x8c386928;scenario.stack=0x8c00ef90;}
        equivalent(scenario,true);
    }
}
TEST_F(GuardedPollTraceTest, OpcodeAndFoldedDelayGuardsRejectOtherPrograms) {
    Scenario first;first.firstOpcode=0xd308;equivalent(first,false);
    Scenario flag;flag.flagOpcode=0x2330;equivalent(flag,false);
    Scenario delay;delay.flag=1;delay.branchDelay=0xe400;delay.change=Change::StackFlag;equivalent(delay,false);
    Scenario island;island.twoHop=true;island.flag=1;island.islandDelay=0xe405;island.change=Change::StackFlag;equivalent(island,false);
}
TEST_F(GuardedPollTraceTest, NonRamAndWrappingStackUseGenericMemoryPath) {
    Scenario aram;aram.pollAddress=0x00800000;equivalent(aram,false);
    Scenario wrapping;wrapping.stack=0x8cfffff0;equivalent(wrapping,false);
}
TEST_F(GuardedPollTraceTest, MmuModeRejectsTraceAndCacheResetDoesNotRetainSelection) {
    Scenario mmu;mmu.mmu=true;equivalent(mmu,false);
    Scenario matching;matching.flag=1;matching.change=Change::StackFlag;equivalent(matching,true);
    Scenario changed;changed.flagOpcode=0x2330;equivalent(changed,false);
}
TEST_F(GuardedPollTraceTest, DISABLED_ActualGameTraceBenchmark) {
#ifndef NDEBUG
    GTEST_SKIP()<<"fixed-work measurements require a Release build";
#endif
    if(threadCpuNs()==0)GTEST_SKIP()<<"thread CPU clock unavailable";
    const char* destination=std::getenv("HOLLYCAST_SH4_TRACE_BENCHMARK_OUTPUT");
    ASSERT_NE(destination,nullptr)<<"set HOLLYCAST_SH4_TRACE_BENCHMARK_OUTPUT to an ignored JSON output path";
    struct Options {
        bool threaded=config::ThreadedRendering,renderer=rend_is_enabled(),fastForward=settings.input.fastForwardMode;
        Options(){TermAudio();config::ThreadedRendering=true;rend_enable_renderer(false);settings.input.fastForwardMode=false;}
        ~Options(){config::ThreadedRendering=threaded;rend_enable_renderer(renderer);settings.input.fastForwardMode=fastForward;}
    } options;
    Scenario scenario;scenario.twoHop=true;scenario.pollAddress=0x8c386928;scenario.stack=0x8c00ef90;
    scenario.flag=1;scenario.benchmark=true;
    // Discard one complete run of each mode, in addition to each sample's
    // continuous untimed warm phase. No audio backend or rendering is active;
    // ordinary AICA, PVR and SH4 scheduler work is intentionally retained.
    const auto reference=run(false,scenario);
    const auto warmTrace=run(true,scenario);
    EXPECT_EQ(warmTrace.final,reference.final);
    EXPECT_EQ(warmTrace.measurement.digest,reference.measurement.digest);
    EXPECT_EQ(warmTrace.measurement.guestCycles,reference.measurement.guestCycles);
    nlohmann::json output={{"schema","hollycast-guarded-poll-fixed-work-v1"},
        {"requested_guest_cycles",SH4_MAIN_CLOCK*2ULL},{"warmup_guest_cycles_min",SH4_MAIN_CLOCK/2},
        {"callback_interval_cycles",8192},{"samples_per_mode",7},
        {"method","same Release test binary; config off/on; precompile with CpuRunning true; continuous untimed warm then fixed guest-cycle CPU interval; normal scheduler/AICA/PVR work retained; audio backend closed and renderer disabled"},
        {"samples",nlohmann::json::array()}};
    for(unsigned round=0;round<7;round++)for(bool enable:{bool(round&1),!bool(round&1)}) {
        const auto result=run(enable,scenario);
        EXPECT_EQ(result.final,reference.final);
        EXPECT_EQ(result.stackBytes,reference.stackBytes);
        EXPECT_EQ(result.measurement.digest,reference.measurement.digest);
        EXPECT_EQ(result.measurement.guestCycles,reference.measurement.guestCycles);
        EXPECT_EQ(result.measurement.callbacks,reference.measurement.callbacks);
        EXPECT_EQ(result.final.r[7],0u);
        if(enable)EXPECT_GT(result.measurement.traceEntries,0u);else EXPECT_EQ(result.measurement.traceEntries,0u);
        output["samples"].push_back({{"round",round+1},{"guarded_trace",enable},
            {"thread_cpu_seconds",result.measurement.threadCpuSeconds},{"wall_seconds",result.measurement.wallSeconds},
            {"guest_cycles",result.measurement.guestCycles},{"scheduler_callbacks",result.measurement.callbacks},
            {"scheduler_digest",result.measurement.digest},{"trace_entries",result.measurement.traceEntries}});
    }
    output["all_context_and_scheduler_checks_passed"]=!HasFailure();
    std::ofstream file(destination);
    ASSERT_TRUE(file.good());file<<output.dump(2)<<'\n';file.close();
    std::cout<<"SH4_GUARDED_TRACE_BENCHMARK "<<destination<<'\n';
}
} // namespace
#else
TEST(GuardedPollTraceTest, RequiresArm64JitAndTestAutomation) {
    GTEST_SKIP()<<"ARM64 JIT trace execution tests require TEST_AUTOMATION=ON";
}
#endif
