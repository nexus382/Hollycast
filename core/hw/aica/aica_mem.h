#pragma once
#include "types.h"

namespace aica
{

template<typename T> T readRegInternal(u32 addr);
template<typename T> void writeRegInternal(u32 addr, T data);

void initMem();
void termMem();

#ifdef USE_HIKARU
alignas(4) extern u8 default_aica_reg[0x8000];
using RegisterBank = u8[0x8000];
RegisterBank& activeRegisters();
void selectRegisters(u8* bank);
#define aica_reg activeRegisters()
#else
alignas(4) extern u8 aica_reg[0x8000];
#endif

}
