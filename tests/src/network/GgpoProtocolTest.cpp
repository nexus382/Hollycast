// Portions Copyright 2026 The Hollycast Authors
#include <gtest/gtest.h>
#include "ggpo/lib/ggpo/network/udp_proto.h"

namespace
{
class TestUdpProtocol : public UdpProtocol
{
public:
	using UdpProtocol::OnAppData;
};

TEST(GgpoProtocolTest, PreservesBufferedAppEventsInOrder)
{
	TestUdpProtocol protocol;
	constexpr int events = 80;
	for (int sequence = 0; sequence < events; ++sequence)
	{
		UdpMsg packet(UdpMsg::AppData);
		packet.u.app_data.spectators = false;
		packet.u.app_data.size = 1;
		packet.u.app_data.data[0] = static_cast<uint8>(sequence);
		ASSERT_TRUE(protocol.OnAppData(&packet, packet.PacketSize()));
	}

	for (int sequence = 0; sequence < events; ++sequence)
	{
		UdpProtocol::Event event;
		ASSERT_TRUE(protocol.GetEvent(event));
		ASSERT_EQ(UdpProtocol::Event::AppData, event.type);
		ASSERT_EQ(1, event.u.app_data.size);
		EXPECT_EQ(sequence, event.u.app_data.data[0]);
	}
	UdpProtocol::Event event;
	EXPECT_FALSE(protocol.GetEvent(event));
}
}
