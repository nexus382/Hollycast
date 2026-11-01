#pragma once
#include "types.h"

namespace aica::sgc
{

void AICA_Sample();
// Generate a board sample without Dreamcast CD audio, VMU beep or host output.
void boardSample(s16& left, s16& right);

// Save channel runtime state when multiplexing independent sound boards.
// Register storage and DSP state must be switched separately; execution and
// state switches must be serialized. These snapshots do not include wave RAM.
struct ChannelState;
ChannelState* createChannelState();
void destroyChannelState(ChannelState* state);
void saveChannelState(ChannelState& state);
void loadChannelState(const ChannelState& state);
void resetBoardChannels();
#ifdef USE_HIKARU
// nullptr selects the stable Dreamcast bank. Select its matching register bank
// first, and restore the previous channel selection before destroying a bank.
ChannelState* activeChannelState();
void selectChannelState(ChannelState* state);
#endif

void WriteChannelReg(u32 channel, u32 reg, int size);

void init();
void term();

union fp_22_10
{
	struct
	{
		u32 fp:10;
		u32 ip:22;
	};
	u32 full;
};
union fp_s_22_10
{
	struct
	{
		u32 fp:10;
		s32 ip:22;
	};
	s32 full;
};
union fp_20_12
{
	struct
	{
		u32 fp:12;
		u32 ip:20;
	};
	u32 full;
};

typedef s32 SampleType;

void ReadCommonReg(u32 reg, bool byte);
void serialize(Serializer& ctx);
void deserialize(Deserializer& ctx);
void vmuBeep(int on, int period);

} // namespace aica::sgc
