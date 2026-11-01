// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
#include "stdclass.h"

namespace aica
{

extern RamRegion aica_ram;
inline u32 ramSize() { return static_cast<u32>(aica_ram.getSize()); }
inline u32 ramMask() { return ramSize() - 1; }
extern int aica_schid;
extern int rtc_schid;
extern int dma_sched_id;
u32 GetRTC_now();
template<typename T> T readRtcReg(u32 addr);
template<typename T> void writeRtcReg(u32 addr, T data);
template<typename T> T readAicaReg(u32 addr);
template<typename T> void writeAicaReg(u32 addr, T data);

void init();
void reset(bool hard);
void term();
void timeStep();
// Selects the interpreter-only arcade host while a board context is active.
// Returns the previous selection; callers must restore it before Dreamcast runs.
bool setBoardHost(bool active);
bool isBoardHost();
bool boardTimeStep(s16& left, s16& right);
bool boardInterruptPending();
void serialize(Serializer& ser);
void deserialize(Deserializer& deser);

void initRtc();
void resetRtc(bool hard);
void termRtc();

void setMidiReceiver(void (*handler)(u8 data));
void midiSend(u8 data);

void sbInit();
void sbReset(bool hard);
void sbTerm();

}
