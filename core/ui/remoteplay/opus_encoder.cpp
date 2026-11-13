// Portions Copyright 2026 The Hollycast Authors
#include "opus_encoder.h"
#include <SDL.h>
#include <opus.h>
#include <array>

namespace remoteplay {
namespace {
class Encoder final : public OpusAudioEncoder
{
public:
	Encoder(PacketCallback callback) : callback(std::move(callback))
	{
		int error;
		encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
		stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, 44100, AUDIO_S16SYS, 2, 48000);
		if (encoder) {
			opus_encoder_ctl(encoder, OPUS_SET_BITRATE(96000));
			opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
		}
	}
	~Encoder() override {
		if (stream) SDL_FreeAudioStream(stream);
		if (encoder) opus_encoder_destroy(encoder);
	}
	bool valid() const { return encoder && stream; }
	void encode(const s16 *stereo, size_t frames) override
	{
		if (SDL_AudioStreamPut(stream, stereo, (int)(frames * 2 * sizeof(s16))) < 0)
			return;
		std::array<s16, 960 * 2> pcm;
		std::array<u8, 4096> packet;
		while (SDL_AudioStreamAvailable(stream) >= (int)sizeof(pcm))
		{
			if (SDL_AudioStreamGet(stream, pcm.data(), sizeof(pcm)) != sizeof(pcm)) break;
			int bytes = opus_encode(encoder, pcm.data(), 960, packet.data(), packet.size());
			if (bytes > 0) callback(std::vector<u8>(packet.begin(), packet.begin() + bytes));
		}
	}
private:
	OpusEncoder *encoder = nullptr;
	SDL_AudioStream *stream = nullptr;
	PacketCallback callback;
};
}
std::unique_ptr<OpusAudioEncoder> OpusAudioEncoder::create(PacketCallback callback)
{
	auto encoder = std::make_unique<Encoder>(std::move(callback));
	if (!encoder->valid()) return nullptr;
	return encoder;
}
}
