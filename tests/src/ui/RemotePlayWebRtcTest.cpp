// Portions Copyright 2026 The Hollycast Authors
#ifdef HOLLYCAST_HAVE_WEBRTC
#include "gtest/gtest.h"
#include "ui/remoteplay/webrtc_peer.h"
#include "ui/remoteplay/h264_encoder.h"
#include "ui/remoteplay/audio_stream.h"
#include "audio/audiostream.h"
#include "json.hpp"
#include <rtc/rtc.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <thread>
#include <cmath>

TEST(RemotePlayTest, WebRtcDeliversVideoAudioAndDirectInput)
{
	using Json = nlohmann::json;
	using namespace std::chrono_literals;
	struct State {
		std::mutex mutex;
		std::queue<std::pair<bool, Json>> signaling;
		std::atomic<int> packets { 0 };
		std::atomic<int> audioPackets { 0 };
		std::atomic<bool> inputReceived { false };
		std::atomic<bool> pongReceived { false };
		std::vector<std::shared_ptr<rtc::Track>> tracks;
		std::shared_ptr<rtc::DataChannel> input;
		void push(bool toHost, Json message) {
			std::lock_guard<std::mutex> lock(mutex);
			signaling.emplace(toHost, std::move(message));
		}
	};
	auto state = std::make_shared<State>();
	auto receiver = std::make_shared<rtc::PeerConnection>();
	receiver->onLocalDescription([state](rtc::Description description) {
		state->push(true, {{"kind", "answer"}, {"sdp", std::string(description)}});
	});
	receiver->onDataChannel([state](std::shared_ptr<rtc::DataChannel> channel) {
		state->input = channel;
		channel->onOpen([weak = std::weak_ptr<rtc::DataChannel>(channel)] {
			if (auto channel = weak.lock()) {
				channel->send(std::string("{\"t\":\"btn\",\"k\":\"A\",\"p\":1}"));
				channel->send(std::string("{\"t\":\"ping\"}"));
			}
		});
		channel->onMessage([weak = std::weak_ptr<State>(state)](rtc::message_variant message) {
			if (auto state = weak.lock())
				if (auto text = std::get_if<std::string>(&message))
					state->pongReceived = Json::parse(*text).value("t", "") == "pong";
		});
	});
	receiver->onLocalCandidate([state](rtc::Candidate candidate) {
		state->push(true, {{"kind", "candidate"}, {"candidate", std::string(candidate)}, {"mid", candidate.mid()}});
	});
	receiver->onTrack([state](std::shared_ptr<rtc::Track> track) {
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->tracks.push_back(track);
		}
		track->onMessage([weak = std::weak_ptr<State>(state), audio = track->mid() == "audio"](rtc::message_variant message) {
			if (auto state = weak.lock())
				if (auto bytes = std::get_if<rtc::binary>(&message); bytes && bytes->size() >= 12)
					(audio ? state->audioPackets : state->packets)++;
		});
	});
	struct Cleanup {
		std::shared_ptr<rtc::PeerConnection> receiver;
		~Cleanup() { remoteplay::stopAudioStreaming(); remoteplay::rtcSessionStop(3); receiver->close(); }
	} cleanup { receiver };

	remoteplay::rtcSessionStart(3, [state](const std::string& message) {
		state->push(false, Json::parse(message));
	}, [state](const std::string& message) {
		state->inputReceived = Json::parse(message).value("t", "") == "btn";
	});
	remoteplay::startAudioStreaming();
	std::atomic<uint32_t> timestamp { 0 };
	auto encoder = remoteplay::H264Encoder::create(64, 64, 30, 200,
		[&timestamp](const std::vector<u8>& sample, bool keyframe) {
			remoteplay::rtcPushSample(sample, keyframe, timestamp.fetch_add(3000));
		});
	ASSERT_NE(encoder, nullptr);
	std::vector<u8> pixels(64 * 64 * 3, 128);
	auto deadline = std::chrono::steady_clock::now() + 8s;
	u64 sample = 0;
	while (std::chrono::steady_clock::now() < deadline
		&& (state->packets == 0 || state->audioPackets == 0 || !state->inputReceived || !state->pongReceived))
	{
		std::queue<std::pair<bool, Json>> pending;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			pending.swap(state->signaling);
		}
		while (!pending.empty())
		{
			auto [toHost, message] = std::move(pending.front());
			pending.pop();
			if (toHost)
				remoteplay::rtcSessionHandleMessage(3, message.dump());
			else if (message["kind"] == "offer")
				receiver->setRemoteDescription(rtc::Description(message["sdp"].get<std::string>(), "offer"));
			else if (message["kind"] == "candidate")
				receiver->addRemoteCandidate(rtc::Candidate(message["candidate"].get<std::string>(), message["mid"].get<std::string>()));
		}
		if (remoteplay::rtcSessionConnected(3))
		{
			encoder->encodeRgb(pixels.data(), 64, 64);
			for (u32 i = 0; i < SAMPLE_COUNT; i++, sample++)
				WriteSample((s16)(std::sin(sample * 880.0 * 6.2831853 / 44100) * 12000),
					(s16)(std::sin(sample * 440.0 * 6.2831853 / 44100) * 12000));
		}
		std::this_thread::sleep_for(10ms);
	}
	EXPECT_TRUE(remoteplay::rtcSessionConnected(3));
	EXPECT_GT(state->packets.load(), 0) << "Connected peers must deliver encoded RTP video";
	EXPECT_GT(state->audioPackets.load(), 0) << "AICA samples must reach the receiver as Opus RTP audio";
	EXPECT_TRUE(state->inputReceived) << "Controller events must reach the host over WebRTC";
	EXPECT_TRUE(state->pongReceived) << "Input latency probes must use the same direct channel";
}
#endif
