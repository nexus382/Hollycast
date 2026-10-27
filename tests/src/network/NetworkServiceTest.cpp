// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "emulator.h"
#include "hw/bba/rtl8139c.h"
#include "hw/mem/addrspace.h"
#include "network/netservice.h"

namespace
{
class TestService final : public net::modbba::Service
{
public:
	bool start() override { ++startCalls; settings.network.online = true; return true; }
	void stop() override { settings.network.online = false; }
	void writeModem(u8) override {}
	int readModem() override { return 0; }
	int modemAvailable() override { return 0; }
	void receiveEthFrame(const u8 *, u32) override { ++receivedFrames; }
	u32 sendEthFrame(u8 *) override { ++sendPolls; return 0; }

	int startCalls = 0;
	int receivedFrames = 0;
	int sendPolls = 0;
};

class NetworkServiceTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		emu.dc_reset(true);
	}
	void TearDown() override
	{
		net::modbba::setCustomService(nullptr);
		settings.network.online = previousOnline;
	}
	bool previousOnline = settings.network.online;
};
}

TEST_F(NetworkServiceTest, ReceivePollingDoesNotStartServiceButGuestTransmitDoes)
{
	TestService service;
	net::modbba::setCustomService(&service);
	settings.network.online = true;
	EXPECT_EQ(net::modbba::sendEthFrame(nullptr), 0u);
	EXPECT_EQ(service.startCalls, 0);

	settings.network.online = false;
	const u8 frame[] = { 0, 1, 2, 3, 4, 5 };
	net::modbba::receiveEthFrame(frame, sizeof(frame));
	EXPECT_EQ(service.startCalls, 1);
	EXPECT_EQ(service.receivedFrames, 1);
}

TEST_F(NetworkServiceTest, StartIfNeededPreservesAnOnlineCustomService)
{
	TestService service;
	net::modbba::setCustomService(&service);
	settings.network.online = true;
	EXPECT_TRUE(net::modbba::startIfNeeded(true));
	EXPECT_EQ(service.startCalls, 0);

	// A stopped custom service can be restarted without replacing its owner.
	settings.network.online = false;
	EXPECT_TRUE(net::modbba::startIfNeeded(true));
	EXPECT_EQ(service.startCalls, 1);
}

TEST_F(NetworkServiceTest, DisabledReceiverDoesNotStartAndReenableRestartsStoppedService)
{
	TestService service;
	net::modbba::setCustomService(&service);
	settings.network.online = false;
	EXPECT_FALSE(net::modbba::startIfNeeded(false)); // RX off / clock gated
	EXPECT_EQ(service.startCalls, 0);
	EXPECT_TRUE(net::modbba::startIfNeeded(true)); // RX becomes enabled
	EXPECT_EQ(service.startCalls, 1);

	net::modbba::stop();
	EXPECT_FALSE(net::modbba::startIfNeeded(false)); // reset/disable before next poll
	EXPECT_TRUE(net::modbba::startIfNeeded(true)); // immediate re-enable
	EXPECT_EQ(service.startCalls, 2);
}

TEST_F(NetworkServiceTest, Rtl8139ReceiveEnableStartsAndClockResumeRestartsService)
{
	TestService service;
	net::modbba::setCustomService(&service);
	NICConf nicConf{};
	RTL8139State *nic = rtl8139_init(&nicConf);
	pci_rtl8139_realize(PCI_DEVICE(nic));
	rtl8139_reset(nic);

	rtl8139_ioport_write(nic, 0x37, 0x08, 1); // ChipCmd: enable RX
	EXPECT_EQ(service.startCalls, 1);
	net::modbba::stop();
	rtl8139_reset(nic); // GAPS reset also stops the BBA service
	rtl8139_ioport_write(nic, 0x37, 0x08, 1); // Immediate RX re-enable
	EXPECT_EQ(service.startCalls, 2);
	net::modbba::stop();
	rtl8139_ioport_write(nic, 0x5b, 'H', 1); // Gate the RTL8139 clock
	EXPECT_EQ(service.startCalls, 2);
	rtl8139_ioport_write(nic, 0x5b, 'R', 1); // Clock resume with RX enabled
	EXPECT_EQ(service.startCalls, 3);

	rtl8139_destroy(nic);
}
