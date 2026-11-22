// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "network/session_state.h"

// Wire-format coverage for the host-to-guest session state bundle: valid
// bundles must round-trip byte-for-byte, and malformed bundles must be
// rejected without producing partial items.
TEST(SessionStateTest, RoundTripPreservesItems)
{
	std::vector<sessionstate::Item> items;
	sessionstate::Item flash;
	flash.kind = sessionstate::KIND_FLASH;
	flash.data = { 0x10, 0x20, 0x30, 0x40 };
	items.push_back(flash);
	sessionstate::Item vmu;
	vmu.kind = sessionstate::KIND_VMU;
	vmu.bus = 1;
	vmu.port = 5;
	vmu.data.assign(1024, 0xab);
	items.push_back(vmu);

	std::vector<u8> encoded;
	ASSERT_TRUE(sessionstate::encode(items, encoded));

	std::vector<sessionstate::Item> decoded;
	ASSERT_TRUE(sessionstate::decode(encoded.data(), encoded.size(), decoded));
	ASSERT_EQ(decoded.size(), 2u);
	EXPECT_EQ(decoded[0].kind, sessionstate::KIND_FLASH);
	EXPECT_EQ(decoded[0].data, flash.data);
	EXPECT_EQ(decoded[1].kind, sessionstate::KIND_VMU);
	EXPECT_EQ(decoded[1].bus, 1);
	EXPECT_EQ(decoded[1].port, 5);
	EXPECT_EQ(decoded[1].data, vmu.data);
}

TEST(SessionStateTest, RejectsMalformedBundles)
{
	std::vector<sessionstate::Item> items;
	sessionstate::Item flash;
	flash.kind = sessionstate::KIND_FLASH;
	flash.data = { 1, 2, 3 };
	items.push_back(flash);
	std::vector<u8> encoded;
	ASSERT_TRUE(sessionstate::encode(items, encoded));

	std::vector<sessionstate::Item> decoded;
	// Bad magic
	std::vector<u8> bad = encoded;
	bad[0] = 'X';
	EXPECT_FALSE(sessionstate::decode(bad.data(), bad.size(), decoded));
	// Truncated payload
	EXPECT_FALSE(sessionstate::decode(encoded.data(), encoded.size() - 1, decoded));
	// Trailing garbage
	bad = encoded;
	bad.push_back(0);
	EXPECT_FALSE(sessionstate::decode(bad.data(), bad.size(), decoded));
	// Unknown item kind
	bad = encoded;
	bad[8] = 99;
	EXPECT_FALSE(sessionstate::decode(bad.data(), bad.size(), decoded));
	EXPECT_TRUE(decoded.empty());
}
