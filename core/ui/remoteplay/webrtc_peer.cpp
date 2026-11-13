/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// WebRTC host peers (one per phone). Signaling JSON travels over the
// existing input WebSocket; media is peer-to-peer H.264 (STUN, plus an
// optional TURN server from config). See docs/remote-play.md.
#include "webrtc_peer.h"
#include "h264_encoder.h"
#include "log/Log.h"

#ifdef HOLLYCAST_HAVE_WEBRTC
#include "cfg/option.h"

#include <rtc/rtc.hpp>
#endif

#include <atomic>
#include <locale>
#include <map>
#include <mutex>

namespace remoteplay {
namespace {
std::atomic<bool> keyframeRequested { false };
} // namespace

bool rtcKeyframeRequested() { return keyframeRequested; }
void rtcClearKeyframeRequest() { keyframeRequested = false; }

#ifdef HOLLYCAST_HAVE_WEBRTC

namespace {

struct RtcSession
{
	RtcSendCallback send;
	RtcInputCallback input;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::Track> audioTrack;
	std::shared_ptr<rtc::DataChannel> inputChannel;
	std::atomic<bool> connected { false };
};

std::mutex rtcMutex;
std::map<int, std::shared_ptr<RtcSession>> rtcSessions;

std::string jsonEscape(const std::string& value)
{
	std::string out;
	for (char c : value)
	{
		switch (c)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c; break;
		}
	}
	return out;
}

// Extract and unescape a JSON string field ("key":"...") from a message.
bool jsonField(const std::string& msg, const char *key, std::string& out)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t p = msg.find(pat);
	if (p == std::string::npos) return false;
	p = msg.find(':', p + pat.size());
	if (p == std::string::npos) return false;
	p = msg.find('"', p);
	if (p == std::string::npos) return false;
	out.clear();
	for (p++; p < msg.size(); p++)
	{
		char c = msg[p];
		if (c == '"')
			return true;
		if (c == '\\' && p + 1 < msg.size())
		{
			char e = msg[++p];
			switch (e)
			{
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case '/': out += '/'; break;
			case '\\': out += '\\'; break;
			case '"': out += '"'; break;
			default: out += e; break;
			}
		}
		else
			out += c;
	}
	return false;
}

// SDP is locale-independent by definition, but libdatachannel formats
// numbers with the process locale (Hollycast sets one for translations),
// which would turn 90000 into "90,000" and produce invalid SDP.
struct ClassicLocaleGuard
{
	std::locale previous;
	ClassicLocaleGuard() : previous(std::locale::global(std::locale::classic())) {}
	~ClassicLocaleGuard() { std::locale::global(previous); }
};

std::shared_ptr<RtcSession> getSession(int player)
{
	std::lock_guard<std::mutex> lock(rtcMutex);
	auto it = rtcSessions.find(player);
	return it == rtcSessions.end() ? nullptr : it->second;
}

void createPeer(int player, const std::shared_ptr<RtcSession>& session)
{
	rtc::Configuration config;
	config.forceMediaTransport = true;
	config.disableAutoNegotiation = true;
	config.iceServers.emplace_back("stun:stun.l.google.com:19302");
	std::string turnUrl = (std::string)config::RemotePlayTurnUrl;
	if (!turnUrl.empty())
	{
		rtc::IceServer turn(turnUrl);
		turn.username = (std::string)config::RemotePlayTurnUsername;
		turn.password = (std::string)config::RemotePlayTurnPassword;
		config.iceServers.emplace_back(turn);
	}
	auto pc = std::make_shared<rtc::PeerConnection>(config);
	session->pc = pc;
	auto send = session->send;
	// The host offers, so the H.264 payload type and SSRC stay the ones
	// configured on our track (browsers accept them in their answer).
	pc->onLocalDescription([send](rtc::Description description) {
		if (send && description.type() == rtc::Description::Type::Offer)
		{
			ClassicLocaleGuard localeGuard;
			send("{\"t\":\"rtc\",\"kind\":\"offer\",\"sdp\":\"" + jsonEscape(std::string(description)) + "\"}");
		}
	});
	pc->onLocalCandidate([send](rtc::Candidate candidate) {
		if (send)
		{
			ClassicLocaleGuard localeGuard;
			send("{\"t\":\"rtc\",\"kind\":\"candidate\",\"candidate\":\"" + jsonEscape(std::string(candidate))
				+ "\",\"mid\":\"" + jsonEscape(candidate.mid()) + "\"}");
		}
	});
	pc->onStateChange([weak = std::weak_ptr<RtcSession>(session), player](rtc::PeerConnection::State state) {
		auto session = weak.lock();
		if (session == nullptr)
			return;
		session->connected = state == rtc::PeerConnection::State::Connected;
		INFO_LOG(NETWORK, "Remote Play WebRTC player %d state %d", player, (int)state);
		if (session->connected)
			keyframeRequested = true;
	});

	const uint32_t ssrc = 42 + (uint32_t)player;
	rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
	media.addH264Codec(96);
	media.addSSRC(ssrc, "remoteplay", "remoteplay", "video");
	auto track = pc->addTrack(media);
	auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "remoteplay", 96, rtc::H264RtpPacketizer::ClockRate);
	auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::Length, rtpConfig);
	packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
	track->setMediaHandler(packetizer);
	session->track = track;
	rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
	audio.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
	audio.addSSRC(100 + player, "remoteplay", "remoteplay", "audio");
	session->audioTrack = pc->addTrack(audio);
	auto audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(100 + player, "remoteplay", 111, 48000);
	auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioConfig);
	audioPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(audioConfig));
	session->audioTrack->setMediaHandler(audioPacketizer);

	// Controller input follows the same direct route as video instead of
	// taking a second trip through the HTTPS tunnel. Reliable delivery keeps
	// button releases from being lost; signaling stays on the WebSocket.
	session->inputChannel = pc->createDataChannel("input");
	session->inputChannel->onMessage([input = session->input,
		weak = std::weak_ptr<rtc::DataChannel>(session->inputChannel)](rtc::message_variant message) {
		if (auto text = std::get_if<std::string>(&message); text && text->size() <= 4096)
		{
			std::string type;
			if (jsonField(*text, "t", type) && type == "ping")
			{
				if (auto channel = weak.lock()) channel->send(std::string("{\"t\":\"pong\"}"));
			}
			else if (input)
				input(*text);
		}
	});

	{
		ClassicLocaleGuard localeGuard;
		pc->setLocalDescription();
	}
	keyframeRequested = true;
}

} // namespace

bool webrtcAvailable() { return h264EncoderAvailable(); }

void rtcSessionStart(int player, RtcSendCallback send, RtcInputCallback input)
{
	auto session = std::make_shared<RtcSession>();
	session->send = std::move(send);
	session->input = std::move(input);
	{
		std::lock_guard<std::mutex> lock(rtcMutex);
		rtcSessions[player] = session;
	}
	try {
		createPeer(player, session);
	} catch (...) {}
}

void rtcSessionStop(int player)
{
	std::shared_ptr<RtcSession> session;
	{
		std::lock_guard<std::mutex> lock(rtcMutex);
		auto it = rtcSessions.find(player);
		if (it != rtcSessions.end())
		{
			session = it->second;
			rtcSessions.erase(it);
		}
	}
	if (session != nullptr && session->pc != nullptr)
		session->pc->close();
}

void rtcSessionHandleMessage(int player, const std::string& json)
{
	auto session = getSession(player);
	if (session == nullptr)
		return;
	std::string kind;
	if (!jsonField(json, "kind", kind))
		return;
	try {
		if (kind == "answer" && session->pc != nullptr)
		{
			std::string sdp;
			if (jsonField(json, "sdp", sdp) && !sdp.empty())
				session->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
		}
		else if (kind == "candidate" && session->pc != nullptr)
		{
			std::string candidate, mid;
			if (jsonField(json, "candidate", candidate) && !candidate.empty())
			{
				jsonField(json, "mid", mid);
				session->pc->addRemoteCandidate(rtc::Candidate(candidate, mid.empty() ? "video" : mid));
			}
		}
	} catch (...) {
		// Malformed signaling from a phone must never take down the host.
	}
}

bool rtcSessionConnected(int player)
{
	auto session = getSession(player);
	return session != nullptr && session->connected;
}

bool rtcAnySession()
{
	std::lock_guard<std::mutex> lock(rtcMutex);
	return !rtcSessions.empty();
}

bool rtcAnyConnected()
{
	std::lock_guard<std::mutex> lock(rtcMutex);
	for (auto& pair : rtcSessions)
		if (pair.second->connected)
			return true;
	return false;
}

void rtcPushSample(const std::vector<u8>& sample, bool, uint32_t rtpTimestamp)
{
	std::lock_guard<std::mutex> lock(rtcMutex);
	for (auto& pair : rtcSessions)
	{
		auto& session = pair.second;
		bool open = session->track != nullptr && session->track->isOpen();
		if (session->connected && open)
		{
			try {
				session->track->sendFrame((const std::byte *)sample.data(), sample.size(),
					rtc::FrameInfo(rtpTimestamp));
			} catch (const std::exception& e) {
				WARN_LOG(NETWORK, "Remote Play WebRTC sendFrame failed: %s", e.what());
			}
		}
	}
}

void rtcPushAudioSample(const std::vector<u8>& packet, uint32_t timestamp)
{
	std::lock_guard<std::mutex> lock(rtcMutex);
	for (auto& [player, session] : rtcSessions)
		if (session->connected && session->audioTrack && session->audioTrack->isOpen())
			try {
				session->audioTrack->sendFrame((const std::byte *)packet.data(), packet.size(), rtc::FrameInfo(timestamp));
			} catch (const std::exception& e) {
				WARN_LOG(NETWORK, "Remote Play audio send failed: %s", e.what());
			}
}

#else // HOLLYCAST_HAVE_WEBRTC

bool webrtcAvailable() { return false; }
void rtcSessionStart(int, RtcSendCallback, RtcInputCallback) {}
void rtcSessionStop(int) {}
void rtcSessionHandleMessage(int, const std::string&) {}
bool rtcSessionConnected(int) { return false; }
bool rtcAnySession() { return false; }
bool rtcAnyConnected() { return false; }
void rtcPushSample(const std::vector<u8>&, bool, uint32_t) {}
void rtcPushAudioSample(const std::vector<u8>&, uint32_t) {}

#endif // HOLLYCAST_HAVE_WEBRTC

} // namespace remoteplay
