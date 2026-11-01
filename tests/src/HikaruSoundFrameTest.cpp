// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/sound.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/aica_mem.h"
#include "hw/aica/aica.h"
#include "hw/aica/dsp.h"
#include "hw/arm7/arm7.h"
#include "hw/mem/addrspace.h"
#include "emulator.h"
#include "gtest/gtest.h"
#include "serialize.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

class HikaruSoundFrameTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(addrspace::reserve());
        emu.init();
        emu.dc_reset(true);
    }
};

struct Result {
    std::vector<std::array<s16, 2>> pcm;
    std::array<std::vector<u32>, 2> ram{std::vector<u32>(0x800000 / 4), std::vector<u32>(0x800000 / 4)};
    std::vector<u64> reads;
    std::vector<int> irqs;
    std::array<hikaru::Sound::BoardDiagnostics, 2> diag{};
};

static void exercise(bool framed, Result &result, int frames = 10) {
    hikaru::Sound sound([](void *p, s16 l, s16 r) { static_cast<Result *>(p)->pcm.push_back({l, r}); }, &result);
    auto cb = sound.callbacks();
    for (unsigned b = 0; b < 2; ++b) {
        result.ram[b][0] = 0xe2800001;
        result.ram[b][1] = 0xe58f0018;
        result.ram[b][2] = 0xeafffffc;
        for (unsigned i = 0; i < 32; ++i)
            result.ram[b][0x1000 / 4 + i] = b ? 0x10001000 : 0x30003000;
        ASSERT_EQ(cb.attach(cb.opaque, b, reinterpret_cast<u8 *>(result.ram[b].data()), 0x800000), 0);
    }
    ASSERT_EQ(cb.reset(cb.opaque), 0);
    ASSERT_EQ(cb.set_reset(cb.opaque, 0), 0);
    for (unsigned b = 0; b < 2; ++b) {
        for (auto reg : std::vector<std::pair<u32, u32>>{{0x2800, 15},
                                                         {4, 0x1000},
                                                         {0xc, 64},
                                                         {0x10, 31},
                                                         {0x24, 0xf00},
                                                         {0x28, 0x20},
                                                         {0, 0xc200},
                                                         {0x28b4, 0x400},
                                                         {0x2c00, 0}})
            ASSERT_EQ(cb.write(cb.opaque, b, reg.first, 2, reg.second), 0);
    }
    u32 random = 0x13572468;
    auto step = [&](int frame) {
        for (int i = 0; i < 30; ++i) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            unsigned b = random & 1;
            ASSERT_EQ(cb.advance(cb.opaque, 4800 + (random % 1500)), 0);
            if ((random & 7) == 0) {
                ASSERT_EQ(cb.write(cb.opaque, b, 0x28, 2, (random >> 8) & 0xff20), 0);
            }
            if ((random & 15) == 1) {
                u64 value = 0;
                ASSERT_EQ(cb.read(cb.opaque, b, 0x2800, 2, &value), 0);
                result.reads.push_back(value);
            }
            if ((random & 15) == 2)
                result.irqs.push_back(cb.irq(cb.opaque, b));
        }
        if (frame == 2) {
            ASSERT_EQ(cb.set_reset(cb.opaque, 1), 0);
            ASSERT_EQ(cb.set_reset(cb.opaque, 0), 0);
            for (unsigned b = 0; b < 2; ++b)
                ASSERT_EQ(cb.write(cb.opaque, b, 0x2c00, 4, 0), 0);
        }
    };
    for (int frame = 0; frame < frames; ++frame) {
        if (framed) {
            hikaru::Sound::FrameScope scope(sound);
            ASSERT_TRUE(scope.acquired());
            step(frame);
        } else
            step(frame);
    }
    for (unsigned b = 0; b < 2; ++b) {
        ASSERT_TRUE(sound.diagnostics(b, result.diag[b]));
    }
    cb.detach(cb.opaque);
}

TEST_F(HikaruSoundFrameTest, RandomMmioResetIrqAndPcmMatchStandalone) {
    Result reference, framed;
    exercise(false, reference);
    exercise(true, framed);
    EXPECT_FALSE(reference.pcm.empty());
    EXPECT_TRUE(std::any_of(reference.pcm.begin(), reference.pcm.end(), [](const auto &s) { return s[0] || s[1]; }));
    EXPECT_EQ(reference.pcm, framed.pcm);
    EXPECT_EQ(reference.ram, framed.ram);
    EXPECT_EQ(reference.reads, framed.reads);
    EXPECT_EQ(reference.irqs, framed.irqs);
    for (int b = 0; b < 2; ++b) {
        EXPECT_EQ(reference.diag[b].armPc, framed.diag[b].armPc);
        EXPECT_EQ(reference.diag[b].irq, framed.diag[b].irq);
        EXPECT_EQ(reference.diag[b].keys, framed.diag[b].keys);
    }
}

TEST_F(HikaruSoundFrameTest, HostRestoredOnNormalAndExceptionalExit) {
    aica::BoardState original;
    original.save();
    auto previousRam = aica::aica_ram;
    aica::aica_reg[0x42] = 0x5a;
    aica::dsp::state.TEMP[3] = 0x12345;
    aica::timers[0].c_step = 17;
    aica::arm::arm_Reg[4].I = 0xfeedface;
    aica::midiSendBuffer.push_back(0x42);
    auto *initialRam = &aica::aica_ram[0];
    auto size = aica::aica_ram.getSize();
    auto sgcBytes = [] {
        Serializer measure;
        aica::sgc::serialize(measure);
        std::vector<u8> data(measure.size());
        Serializer write(data.data(), data.size());
        aica::sgc::serialize(write);
        return data;
    };
    std::vector<u8> expectedRegs(aica::aica_reg, aica::aica_reg + 0x8000);
    std::vector<u8> expectedRam(initialRam, initialRam + size);
    auto expectedDsp = aica::dsp::state;
    auto expectedMidi = aica::midiSendBuffer;
    auto expectedSgc = sgcBytes();
    aica::arm::ExecutionState expectedArm{};
    aica::arm::saveExecutionState(expectedArm);
    std::array<s32, 3> expectedTimerSteps{};
    std::array<u32, 3> expectedTimerRates{};
    for (int i = 0; i < 3; ++i) {
        expectedTimerSteps[i] = aica::timers[i].c_step;
        expectedTimerRates[i] = aica::timers[i].m_step;
    }
    std::vector<u8> ram0(0x800000), ram1(0x800000);
    hikaru::Sound sound(nullptr, nullptr);
    auto cb = sound.callbacks();
    ASSERT_EQ(cb.attach(cb.opaque, 0, ram0.data(), ram0.size()), 0);
    ASSERT_EQ(cb.attach(cb.opaque, 1, ram1.data(), ram1.size()), 0);
    ASSERT_EQ(cb.reset(cb.opaque), 0);
    ASSERT_EQ(cb.set_reset(cb.opaque, 0), 0);
    auto verifyHost = [&] {
        EXPECT_EQ(aica::aica_reg[0x42], 0x5a);
        EXPECT_EQ(aica::dsp::state.TEMP[3], 0x12345);
        EXPECT_EQ(aica::timers[0].c_step, 17);
        EXPECT_EQ(aica::arm::arm_Reg[4].I, 0xfeedfaceu);
        EXPECT_EQ(aica::midiSendBuffer.back(), 0x42);
        EXPECT_EQ(&aica::aica_ram[0], initialRam);
        EXPECT_EQ(aica::aica_ram.getSize(), size);
        EXPECT_FALSE(aica::isBoardHost());
        EXPECT_TRUE(std::equal(expectedRegs.begin(), expectedRegs.end(), aica::aica_reg));
        EXPECT_TRUE(std::equal(expectedRam.begin(), expectedRam.end(), &aica::aica_ram[0]));
        const auto &actualDsp = aica::dsp::state;
        EXPECT_TRUE(std::equal(std::begin(expectedDsp.TEMP), std::end(expectedDsp.TEMP), std::begin(actualDsp.TEMP)));
        EXPECT_TRUE(std::equal(std::begin(expectedDsp.MEMS), std::end(expectedDsp.MEMS), std::begin(actualDsp.MEMS)));
        EXPECT_TRUE(std::equal(std::begin(expectedDsp.MIXS), std::end(expectedDsp.MIXS), std::begin(actualDsp.MIXS)));
        EXPECT_TRUE(std::equal(std::begin(expectedDsp.MEMVAL), std::end(expectedDsp.MEMVAL), std::begin(actualDsp.MEMVAL)));
        EXPECT_EQ(expectedDsp.RBP, actualDsp.RBP);
        EXPECT_EQ(expectedDsp.RBL, actualDsp.RBL);
        EXPECT_EQ(expectedDsp.MDEC_CT, actualDsp.MDEC_CT);
        EXPECT_EQ(expectedDsp.SHIFTED, actualDsp.SHIFTED);
        EXPECT_EQ(expectedDsp.B, actualDsp.B);
        EXPECT_EQ(expectedDsp.FRC_REG, actualDsp.FRC_REG);
        EXPECT_EQ(expectedDsp.Y_REG, actualDsp.Y_REG);
        EXPECT_EQ(expectedDsp.ADRS_REG, actualDsp.ADRS_REG);
        EXPECT_EQ(expectedDsp.stopped, actualDsp.stopped);
        EXPECT_EQ(expectedDsp.dirty, actualDsp.dirty);
        EXPECT_EQ(expectedMidi, aica::midiSendBuffer);
        EXPECT_EQ(expectedSgc, sgcBytes());
        aica::arm::ExecutionState actualArm{};
        aica::arm::saveExecutionState(actualArm);
        for (unsigned i = 0; i < aica::arm::RN_ARM_REG_COUNT; ++i)
            EXPECT_EQ(expectedArm.registers[i].I, actualArm.registers[i].I);
        EXPECT_EQ(expectedArm.clockTicks, actualArm.clockTicks);
        EXPECT_EQ(expectedArm.mode, actualArm.mode);
        EXPECT_EQ(expectedArm.irqEnable, actualArm.irqEnable);
        EXPECT_EQ(expectedArm.fiqEnable, actualArm.fiqEnable);
        EXPECT_EQ(expectedArm.enabled, actualArm.enabled);
        EXPECT_EQ(expectedArm.aicaInterrupt, actualArm.aicaInterrupt);
        EXPECT_EQ(expectedArm.aicaInterruptLevel, actualArm.aicaInterruptLevel);
        EXPECT_EQ(expectedArm.interruptOutput, actualArm.interruptOutput);
        EXPECT_EQ(expectedArm.interruptLevel, actualArm.interruptLevel);
        EXPECT_EQ(expectedArm.interruptMode, actualArm.interruptMode);
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(expectedTimerSteps[i], aica::timers[i].c_step);
            EXPECT_EQ(expectedTimerRates[i], aica::timers[i].m_step);
        }
    };
    {
        hikaru::Sound::FrameScope scope(sound);
        ASSERT_TRUE(scope.acquired());
        ASSERT_EQ(cb.advance(cb.opaque, 200000), 0);
    }
    verifyHost();
    try {
        hikaru::Sound::FrameScope scope(sound);
        ASSERT_TRUE(scope.acquired());
        ASSERT_EQ(cb.write(cb.opaque, 0, 0x2800, 2, 15), 0);
        throw std::runtime_error("injected");
    } catch (const std::runtime_error &) {
    }
    verifyHost();
    cb.detach(cb.opaque);
    hikaru::Sound failSound([](void *, s16, s16) { throw std::runtime_error("injected sink"); }, nullptr);
    auto fail = failSound.callbacks();
    ASSERT_EQ(fail.attach(fail.opaque, 0, ram0.data(), ram0.size()), 0);
    ASSERT_EQ(fail.attach(fail.opaque, 1, ram1.data(), ram1.size()), 0);
    ASSERT_EQ(fail.reset(fail.opaque), 0);
    ASSERT_EQ(fail.set_reset(fail.opaque, 0), 0);
    {
        hikaru::Sound::FrameScope scope(failSound);
        ASSERT_TRUE(scope.acquired());
        EXPECT_EQ(fail.advance(fail.opaque, 200000), -1);
    }
    verifyHost();
    fail.detach(fail.opaque);
    original.load();
    aica::aica_ram = previousRam;
}

TEST_F(HikaruSoundFrameTest, NestedScopeIsRejectedAndOtherThreadWaits) {
    std::vector<u8> ram0(0x800000), ram1(0x800000);
    hikaru::Sound sound(nullptr, nullptr);
    auto cb = sound.callbacks();
    ASSERT_EQ(cb.attach(cb.opaque, 0, ram0.data(), ram0.size()), 0);
    ASSERT_EQ(cb.attach(cb.opaque, 1, ram1.data(), ram1.size()), 0);
    ASSERT_EQ(cb.reset(cb.opaque), 0);
    std::promise<void> started, finished;
    auto startedFuture = started.get_future();
    auto finishedFuture = finished.get_future();
    std::thread worker;
    {
        hikaru::Sound::FrameScope outer(sound);
        ASSERT_TRUE(outer.acquired());
        {
            hikaru::Sound::FrameScope nested(sound);
            EXPECT_FALSE(nested.acquired());
        }
        worker = std::thread([&] {
            started.set_value();
            cb.irq(cb.opaque, 0);
            finished.set_value();
        });
        startedFuture.wait();
        EXPECT_EQ(finishedFuture.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    }
    worker.join();
    EXPECT_EQ(finishedFuture.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    cb.detach(cb.opaque);
}

TEST_F(HikaruSoundFrameTest, OptionalRealFirmwareMatches) {
    const char *path = std::getenv("HOLLYCAST_HIKARU_SOUND_RAM_IMAGE");
    if (!path || !*path)
        GTEST_SKIP();
    auto run = [&](bool framed, Result &result) {
        FILE *file = std::fopen(path, "rb");
        ASSERT_NE(file, nullptr);
        size_t n = std::fread(result.ram[0].data(), 1, 0x800000, file);
        std::fclose(file);
        ASSERT_EQ(n, 0x800000u);
        result.ram[1] = result.ram[0];
        hikaru::Sound sound([](void *p, s16 l, s16 r) { static_cast<Result *>(p)->pcm.push_back({l, r}); }, &result);
        auto cb = sound.callbacks();
        for (unsigned b = 0; b < 2; ++b)
            ASSERT_EQ(cb.attach(cb.opaque, b, reinterpret_cast<u8 *>(result.ram[b].data()), 0x800000), 0);
        ASSERT_EQ(cb.reset(cb.opaque), 0);
        ASSERT_EQ(cb.set_reset(cb.opaque, 0), 0);
        for (unsigned b = 0; b < 2; ++b)
            ASSERT_EQ(cb.write(cb.opaque, b, 0x2c00, 4, 0), 0);
        for (unsigned f = 0; f < 12; ++f) {
            auto frame = [&] {
                for (unsigned i = 0; i < 544; ++i)
                    ASSERT_EQ(cb.advance(cb.opaque, 6100), 0);
            };
            if (framed) {
                hikaru::Sound::FrameScope scope(sound);
                ASSERT_TRUE(scope.acquired());
                frame();
            } else
                frame();
            for (unsigned b = 0; b < 2; ++b)
                result.irqs.push_back(cb.irq(cb.opaque, b));
        }
        for (unsigned b = 0; b < 2; ++b) {
            ASSERT_EQ(result.ram[b][0x5c / 4], 0x54494e49u);
            ASSERT_TRUE(sound.diagnostics(b, result.diag[b]));
        }
        cb.detach(cb.opaque);
    };
    Result baseline, scoped;
    run(false, baseline);
    run(true, scoped);
    EXPECT_EQ(baseline.ram, scoped.ram);
    EXPECT_EQ(baseline.pcm, scoped.pcm);
    EXPECT_EQ(baseline.irqs, scoped.irqs);
    for (int b = 0; b < 2; ++b)
        EXPECT_EQ(baseline.diag[b].armPc, scoped.diag[b].armPc);
}

TEST_F(HikaruSoundFrameTest, DefaultBankAndMixerSurviveBoardSelection) {
 auto *base=&aica::aica_reg[0];
 auto *common=aica::CommonData; auto *dsp=aica::DSPData; auto *vol=aica::dsp_out_vol;
 aica::BoardState host; host.save();
 std::fill(std::begin(dsp->MPRO),std::end(dsp->MPRO),0);
 dsp->MPRO[1]=0x8000 | (1<<13) | (0x30<<7); dsp->MPRO[6]=0x1000;
 dsp->COEF[0]=0x4000; dsp->EXTS[0]=1024;
 aica::dsp::state.dirty=true; aica::dsp::step();
 const u32 jitExpected=dsp->EFREG[0]; ASSERT_NE(jitExpected,0u);
 std::vector<u8> ram0(0x800000), ram1(0x800000);
 hikaru::Sound sound(nullptr,nullptr); auto cb=sound.callbacks();
 ASSERT_EQ(cb.attach(cb.opaque,0,ram0.data(),ram0.size()),0);
 ASSERT_EQ(cb.attach(cb.opaque,1,ram1.data(),ram1.size()),0);
 ASSERT_EQ(cb.reset(cb.opaque),0); ASSERT_EQ(cb.set_reset(cb.opaque,0),0);
 {
  hikaru::Sound::FrameScope scope(sound); ASSERT_TRUE(scope.acquired());
  ASSERT_EQ(cb.write(cb.opaque,0,0x2800,2,15),0);
  EXPECT_NE(aica::CommonData,common); EXPECT_NE(aica::DSPData,dsp); EXPECT_NE(aica::dsp_out_vol,vol);
  ASSERT_EQ(cb.advance(cb.opaque,30000),0);
 }
 EXPECT_EQ(&aica::aica_reg[0],base); EXPECT_EQ(aica::CommonData,common); EXPECT_EQ(aica::DSPData,dsp); EXPECT_EQ(aica::dsp_out_vol,vol);
 dsp->EFREG[0]=0; aica::dsp::step(); EXPECT_EQ(dsp->EFREG[0],jitExpected);
 s16 l=0,r=0; aica::boardTimeStep(l,r);
 EXPECT_EQ(&aica::aica_reg[0],base); EXPECT_EQ(aica::CommonData,common); EXPECT_EQ(aica::DSPData,dsp);
 cb.detach(cb.opaque); host.load();
}
TEST_F(HikaruSoundFrameTest, NestedOtherSoundRestoresOuterBank) {
 auto run=[](bool nested, Result& result) {
  hikaru::Sound a([](void* p,s16 l,s16 r){static_cast<Result*>(p)->pcm.push_back({l,r});},&result);
  hikaru::Sound b(nullptr,nullptr); auto ca=a.callbacks(); auto cb=b.callbacks();
  std::vector<u8> b0(0x800000),b1(0x800000);
  for(unsigned i=0;i<2;i++) {
   result.ram[i][0]=0xe2800001; result.ram[i][1]=0xeafffffd;
   for(unsigned j=0;j<32;j++) result.ram[i][0x1000/4+j]=0x30003000;
   ASSERT_EQ(ca.attach(ca.opaque,i,(u8*)result.ram[i].data(),0x800000),0);
  }
  ASSERT_EQ(cb.attach(cb.opaque,0,b0.data(),b0.size()),0); ASSERT_EQ(cb.attach(cb.opaque,1,b1.data(),b1.size()),0);
  ASSERT_EQ(ca.reset(ca.opaque),0); ASSERT_EQ(ca.set_reset(ca.opaque,0),0);
  for(unsigned i=0;i<2;i++) for(auto reg:std::vector<std::pair<u32,u32>>{{0x2800,15},{4,0x1000},{0xc,64},{0x10,31},{0x24,0xf00},{0x28,0x20},{0,0xc200},{0x2c00,0}})
   ASSERT_EQ(ca.write(ca.opaque,i,reg.first,2,reg.second),0);
  auto *defaultBank=&aica::aica_reg[0];
  {
   hikaru::Sound::FrameScope scope(a); ASSERT_TRUE(scope.acquired());
   ASSERT_EQ(ca.advance(ca.opaque,40000),0);
   auto *outerBank=&aica::aica_reg[0];
   if(nested) {
    ASSERT_EQ(cb.reset(cb.opaque),0); ASSERT_EQ(cb.set_reset(cb.opaque,0),0);
    ASSERT_EQ(cb.write(cb.opaque,0,0x2800,2,3),0); ASSERT_EQ(cb.advance(cb.opaque,30000),0);
    EXPECT_EQ(&aica::aica_reg[0],outerBank);
   }
   ASSERT_EQ(ca.advance(ca.opaque,60000),0);
  }
  EXPECT_EQ(&aica::aica_reg[0],defaultBank);
  for(unsigned i=0;i<2;i++) {ASSERT_TRUE(a.diagnostics(i,result.diag[i]));}
  ca.detach(ca.opaque); cb.detach(cb.opaque);
 };
 Result expected,actual; run(false,expected); run(true,actual);
 EXPECT_EQ(expected.ram,actual.ram); EXPECT_EQ(expected.pcm,actual.pcm);
}

TEST_F(HikaruSoundFrameTest, GenericSnapshotPreservesSelectedChannelBankAndSerialization)
{
    auto serializeChannels = [] {
        Serializer measure; aica::sgc::serialize(measure);
        std::vector<u8> bytes(measure.size());
        Serializer writer(bytes.data(), bytes.size()); aica::sgc::serialize(writer);
        return bytes;
    };
    auto *defaultBank = aica::sgc::activeChannelState();
    EXPECT_EQ(defaultBank, nullptr);
    {
        std::vector<u8> ram0(0x800000), ram1(0x800000);
        hikaru::Sound sound(nullptr, nullptr);
        auto cb = sound.callbacks();
        ASSERT_EQ(cb.attach(cb.opaque, 0, ram0.data(), ram0.size()), 0);
        ASSERT_EQ(cb.attach(cb.opaque, 1, ram1.data(), ram1.size()), 0);
        ASSERT_EQ(cb.reset(cb.opaque), 0);
        ASSERT_EQ(cb.set_reset(cb.opaque, 0), 0);
        {
            hikaru::Sound::FrameScope scope(sound);
            ASSERT_TRUE(scope.acquired());
            ASSERT_EQ(cb.write(cb.opaque, 0, 0x2800, 2, 15), 0);
            ASSERT_EQ(cb.write(cb.opaque, 0, 0, 2, 0xc200), 0);
            auto *selected = aica::sgc::activeChannelState();
            ASSERT_NE(selected, nullptr);
            aica::BoardState snapshot;
            snapshot.save();
            auto saved = serializeChannels();
            aica::writeAicaReg<u16>(0x28, 0x1234);
            aica::writeAicaReg<u16>(0, 0x4000);
            snapshot.load();
            EXPECT_EQ(aica::sgc::activeChannelState(), selected);
            EXPECT_EQ(serializeChannels(), saved);
            u64 value = 0;
            ASSERT_EQ(cb.read(cb.opaque, 0, 0x2800, 2, &value), 0);
            EXPECT_EQ(value & 15, 15u);
            EXPECT_EQ(aica::sgc::activeChannelState(), selected);
        }
        EXPECT_EQ(aica::sgc::activeChannelState(), defaultBank);
        cb.detach(cb.opaque);
    }
    EXPECT_EQ(aica::sgc::activeChannelState(), defaultBank);
}
