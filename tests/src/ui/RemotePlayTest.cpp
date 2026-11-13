// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/remoteplay/remote_play_protocol.h"
#include "ui/remoteplay/cloudflare_tunnel.h"

using namespace remoteplay;

TEST(RemotePlayTest, WebSocketAcceptKeyMatchesRfcExample)
{
	// RFC 6455 section 1.3 worked example.
	EXPECT_EQ(webSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(RemotePlayTest, Sha1MatchesKnownVector)
{
	Sha1 sha;
	const char *msg = "abc";
	sha.update((const u8 *)msg, 3);
	auto digest = sha.finish();
	EXPECT_EQ(base64Encode(digest.data(), digest.size()), "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
}

TEST(RemotePlayTest, EncodedFrameParsesBackWhenMasked)
{
	std::string frame = wsEncodeFrame(WsOpcode::Text, "hello");
	ASSERT_GE(frame.size(), 2u);
	EXPECT_EQ(frame[0], (char)0x81);
	EXPECT_EQ(frame[1], 5);
	EXPECT_EQ(frame.substr(2), "hello");

	// Client frames arrive masked; build one and parse it back.
	std::string masked;
	masked += (char)0x81;
	masked += (char)(0x80 | 5);
	const char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	masked.append(mask, 4);
	const char *payload = "hello";
	for (int i = 0; i < 5; i++)
		masked += (char)(payload[i] ^ mask[i % 4]);
	WsFrame parsed;
	EXPECT_EQ(wsParseFrame((const u8 *)masked.data(), masked.size(), parsed), (int)masked.size());
	EXPECT_EQ(parsed.opcode, WsOpcode::Text);
	EXPECT_EQ(parsed.payload, "hello");

	// Unmasked client frames are a protocol error.
	EXPECT_EQ(wsParseFrame((const u8 *)frame.data(), frame.size(), parsed), -1);
	// A truncated frame asks for more bytes.
	EXPECT_EQ(wsParseFrame((const u8 *)masked.data(), 3, parsed), 0);
}

TEST(RemotePlayTest, PinComparisonRejectsWrongEmptyAndShort)
{
	EXPECT_TRUE(pinMatches("123456", "123456"));
	EXPECT_FALSE(pinMatches("123457", "123456"));
	EXPECT_FALSE(pinMatches("12345", "123456"));
	EXPECT_FALSE(pinMatches("", "123456"));
	EXPECT_FALSE(pinMatches("123456", ""));
}

TEST(RemotePlayTest, PlayerSlotsFillReleaseAndReuse)
{
	PlayerSlots slots;
	EXPECT_EQ(slots.claim(-1), 0);
	EXPECT_EQ(slots.claim(-1), 1);
	EXPECT_EQ(slots.claim(3), 3);
	EXPECT_EQ(slots.claim(3), -1);	// taken
	EXPECT_EQ(slots.claim(2), 2);
	EXPECT_EQ(slots.claim(-1), -1);	// full
	EXPECT_EQ(slots.usedCount(), 4);
	slots.release(1);
	EXPECT_FALSE(slots.isUsed(1));
	EXPECT_EQ(slots.claim(-1), 1);	// reused
	slots.release(9);				// out of range: no-op
	EXPECT_EQ(slots.claim(9), -1);
}

TEST(RemotePlayTest, PlayerStatusTracksJoiningConnectionAndDisconnect)
{
	PlayerSlots slots;
	EXPECT_EQ(slots.snapshot()[0], PlayerState::Available);
	EXPECT_FALSE(slots.connect(0)); // A connection needs a PIN reservation.
	ASSERT_EQ(slots.claim(0), 0);
	EXPECT_EQ(slots.snapshot()[0], PlayerState::Joining);
	ASSERT_TRUE(slots.connect(0));
	EXPECT_EQ(slots.snapshot()[0], PlayerState::Connected);
	EXPECT_FALSE(slots.connect(0)); // A duplicate socket must not own the same port.
	ASSERT_EQ(slots.claim(2), 2);
	ASSERT_TRUE(slots.connect(2));
	slots.release(0);
	EXPECT_EQ(slots.snapshot()[0], PlayerState::Available);
	EXPECT_EQ(slots.snapshot()[2], PlayerState::Connected);
	ASSERT_EQ(slots.claim(0), 0);
	EXPECT_EQ(slots.snapshot()[0], PlayerState::Joining);
	EXPECT_FALSE(slots.connect(-1));
	EXPECT_FALSE(slots.connect(4));
	slots.reset();
	for (PlayerState state : slots.snapshot())
		EXPECT_EQ(state, PlayerState::Available);
}

TEST(RemotePlayTest, ControlNamesMapToDreamcastKeys)
{
	EXPECT_EQ(remoteControlFromName("A"), (u32)DC_BTN_A);
	EXPECT_EQ(remoteControlFromName("START"), (u32)DC_BTN_START);
	EXPECT_EQ(remoteControlFromName("UP"), (u32)DC_DPAD_UP);
	EXPECT_EQ(remoteControlFromName("LT"), (u32)DC_AXIS_LT);
	EXPECT_EQ(remoteControlFromName("NOPE"), 0u);
	EXPECT_EQ(remoteControlFromName(""), 0u);
}

TEST(RemotePlayTest, ParsesButtonStickTriggerAndRelease)
{
	RemoteInputEvent event;
	ASSERT_TRUE(parseRemoteInputMessage("{\"t\":\"btn\",\"k\":\"A\",\"p\":1}", event));
	EXPECT_EQ(event.type, RemoteInputEvent::Type::Button);
	EXPECT_EQ(event.key, (u32)DC_BTN_A);
	EXPECT_TRUE(event.pressed);

	ASSERT_TRUE(parseRemoteInputMessage("{\"t\":\"stick\",\"x\":-0.5,\"y\":2}", event));
	EXPECT_EQ(event.type, RemoteInputEvent::Type::Stick);
	EXPECT_FLOAT_EQ(event.x, -0.5f);
	EXPECT_FLOAT_EQ(event.y, 1.0f);	// clamped

	ASSERT_TRUE(parseRemoteInputMessage("{\"t\":\"axis\",\"k\":\"RT\",\"v\":0.25}", event));
	EXPECT_EQ(event.type, RemoteInputEvent::Type::Trigger);
	EXPECT_FLOAT_EQ(event.value, 0.25f);

	ASSERT_TRUE(parseRemoteInputMessage("{\"t\":\"release\"}", event));
	EXPECT_EQ(event.type, RemoteInputEvent::Type::ReleaseAll);
}

TEST(RemotePlayTest, RejectsMalformedInputMessages)
{
	RemoteInputEvent event;
	EXPECT_FALSE(parseRemoteInputMessage("", event));
	EXPECT_FALSE(parseRemoteInputMessage("not json", event));
	EXPECT_FALSE(parseRemoteInputMessage("{\"t\":\"btn\",\"k\":\"NOPE\",\"p\":1}", event));
	EXPECT_FALSE(parseRemoteInputMessage("{\"t\":\"btn\",\"k\":\"A\"}", event));
	EXPECT_FALSE(parseRemoteInputMessage("{\"t\":\"axis\",\"k\":\"A\",\"v\":1}", event));
	EXPECT_FALSE(parseRemoteInputMessage("{\"t\":\"btn\",\"k\":\"LT\",\"p\":1}", event));
	EXPECT_FALSE(parseRemoteInputMessage("{\"t\":\"explode\"}", event));
}

TEST(RemotePlayTest, CloudflaredDownloadUrlIsOfficialReleaseForPlatform)
{
	std::string url = cloudflaredDownloadUrl();
	EXPECT_EQ(url.rfind("https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-", 0), 0u);
#if defined(__APPLE__) && defined(__aarch64__)
	EXPECT_NE(url.find("cloudflared-darwin-arm64.tgz"), std::string::npos);
#endif
}

TEST(RemotePlayTest, InstalledCloudflaredPathLivesInDataFolder)
{
	std::string path = installedCloudflaredPath();
#ifdef _WIN32
	EXPECT_NE(path.find("cloudflared.exe"), std::string::npos);
#else
	EXPECT_EQ(path.substr(path.size() - 11), "cloudflared");
#endif
}

TEST(RemotePlayTest, ParsesQuickTunnelUrlFromCloudflaredOutput)
{
	EXPECT_EQ(parseQuickTunnelUrl("INF https://brave-river-1234.trycloudflare.com |"), "https://brave-river-1234.trycloudflare.com");
	EXPECT_EQ(parseQuickTunnelUrl("no url here"), "");
	EXPECT_EQ(parseQuickTunnelUrl("https://example.com is not it"), "");
}
