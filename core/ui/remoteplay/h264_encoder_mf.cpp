/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Flycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Flycast. If not, see <https://www.gnu.org/licenses/>.
*/
#include "h264_encoder.h"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace remoteplay {
namespace {

using Microsoft::WRL::ComPtr;

// The capture thread creates, uses and destroys the encoder.
class MediaFoundation
{
public:
	MediaFoundation()
		: comResult(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
	{
		if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
			started = SUCCEEDED(MFStartup(MF_VERSION));
	}

	~MediaFoundation()
	{
		if (started)
			MFShutdown();
		if (SUCCEEDED(comResult))
			CoUninitialize();
	}

	MediaFoundation(const MediaFoundation&) = delete;
	MediaFoundation& operator=(const MediaFoundation&) = delete;

	bool ready() const { return started; }

private:
	HRESULT comResult;
	bool started = false;
};

HRESULT createTransform(ComPtr<IMFTransform>& transform)
{
	return CoCreateInstance(__uuidof(CMSH264EncoderMFT), nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(transform.ReleaseAndGetAddressOf()));
}

ComPtr<IMFMediaType> videoType(const GUID& subtype, int width, int height, int fps)
{
	ComPtr<IMFMediaType> type;
	if (FAILED(MFCreateMediaType(type.GetAddressOf()))
			|| FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
			|| FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype))
			|| FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height))
			|| FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1))
			|| FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))
			|| FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)))
		return {};
	return type;
}

HRESULT setCodecUInt32(ICodecAPI *codec, const GUID& property, ULONG value)
{
	VARIANT setting{};
	setting.vt = VT_UI4;
	setting.ulVal = value;
	return codec->SetValue(&property, &setting);
}

void rgbToNv12(const u8 *rgb, int width, int height, u8 *nv12, int stride, int paddedHeight)
{
	u8 *chroma = nv12 + static_cast<size_t>(stride) * paddedHeight;
	const size_t srcStride = static_cast<size_t>(width) * 3;
	// Replicate the edge pixel if an RGB dimension is odd. Chroma is
	// averaged over each 2x2 block using limited-range BT.601 coefficients.
	for (int y = 0; y < paddedHeight; y += 2)
	{
		const u8 *top = rgb + static_cast<size_t>(y) * srcStride;
		const u8 *bottom = top + (y + 1 < height ? srcStride : 0);
		u8 *topY = nv12 + static_cast<size_t>(y) * stride;
		u8 *bottomY = topY + stride;
		u8 *uv = chroma + static_cast<size_t>(y / 2) * stride;
		for (int x = 0; x < stride; x += 2)
		{
			const size_t left = static_cast<size_t>(x) * 3;
			const size_t right = left + (x + 1 < width ? 3 : 0);
			const u8 *pixels[] = {top + left, top + right, bottom + left, bottom + right};
			u8 *luma[] = {topY + x, topY + x + 1, bottomY + x, bottomY + x + 1};
			int red = 0, green = 0, blue = 0;
			for (size_t i = 0; i < 4; ++i)
			{
				const u8 *pixel = pixels[i];
				*luma[i] = static_cast<u8>(16 + (66 * pixel[0] + 129 * pixel[1] + 25 * pixel[2] + 128) / 256);
				red += pixel[0];
				green += pixel[1];
				blue += pixel[2];
			}
			uv[x] = static_cast<u8>((128 * 1024 - 38 * red - 74 * green + 112 * blue + 512) / 1024);
			uv[x + 1] = static_cast<u8>((128 * 1024 + 112 * red - 94 * green - 18 * blue + 512) / 1024);
		}
	}
}

// Start codes terminate at 01 after at least two zeros. Any extra zero
// bytes before the code belong to Annex B framing, not the preceding NAL.
template<class Callback>
void forEachAnnexBNal(const u8 *data, size_t size, Callback callback)
{
	size_t start = size;
	size_t zeros = 0;
	for (size_t i = 0; i < size; ++i)
	{
		if (data[i] == 0)
		{
			++zeros;
			continue;
		}
		if (data[i] == 1 && zeros >= 2)
		{
			if (start < i - zeros)
				callback(data + start, i - zeros - start);
			start = i + 1;
		}
		zeros = 0;
	}
	if (start < size - zeros)
		callback(data + start, size - zeros - start);
}

void appendAvcc(std::vector<u8>& sample, const u8 *nal, size_t size)
{
	for (int shift = 24; shift >= 0; shift -= 8)
		sample.push_back(static_cast<u8>(size >> shift));
	sample.insert(sample.end(), nal, nal + size);
}

class H264EncoderMf final : public H264Encoder
{
public:
	H264EncoderMf(int fps, int bitrateKbps, SampleCallback callback)
		: fps(fps), bitrateKbps(bitrateKbps), callback(std::move(callback))
	{}

	~H264EncoderMf() override
	{
		resetTransform();
	}

	bool configure(int width, int height)
	{
		if (!mediaFoundation.ready() || width <= 0 || height <= 0
				|| width == (std::numeric_limits<int>::max)() || height == (std::numeric_limits<int>::max)())
			return false;

		const int stride = (width + 1) & ~1;
		const int paddedHeight = (height + 1) & ~1;
		const uint64_t size = static_cast<uint64_t>(stride) * paddedHeight * 3 / 2;
		if (size > (std::numeric_limits<DWORD>::max)())
			return false;

		ComPtr<IMFTransform> next;
		ComPtr<ICodecAPI> nextCodec;
		if (FAILED(createTransform(next)) || FAILED(next.As(&nextCodec)))
			return false;

		// Baseline excludes B frames. The software encoder uses low-latency
		// slice encoding by default, but set these explicitly when supported.
		VARIANT lowLatency{};
		lowLatency.vt = VT_BOOL;
		lowLatency.boolVal = VARIANT_TRUE;
		nextCodec->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency);
		setCodecUInt32(nextCodec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
		setCodecUInt32(nextCodec.Get(), CODECAPI_AVEncMPVGOPSize, fps * 2);

		auto output = videoType(MFVideoFormat_H264, stride, paddedHeight, fps);
		auto input = videoType(MFVideoFormat_NV12, stride, paddedHeight, fps);
		// This MFT requires the compressed output type before its input type.
		if (!output || !input
				|| FAILED(output->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000u))
				|| FAILED(output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base))
				|| FAILED(next->SetOutputType(0, output.Get(), 0))
				|| FAILED(input->SetUINT32(MF_MT_DEFAULT_STRIDE, stride))
				|| FAILED(input->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(size)))
				|| FAILED(input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601))
				|| FAILED(input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235))
				|| FAILED(next->SetInputType(0, input.Get(), 0))
				|| FAILED(next->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)))
			return false;

		if (FAILED(next->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
		{
			next->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
			return false;
		}

		transform = std::move(next);
		codec = std::move(nextCodec);
		readSequenceHeader();
		sourceWidth = width;
		sourceHeight = height;
		encodedWidth = stride;
		encodedHeight = paddedHeight;
		inputBytes = static_cast<DWORD>(size);
		forceKeyframe = true;
		return true;
	}

	void requestKeyframe() override
	{
		forceKeyframe = true;
	}

	void encodeRgb(const u8 *rgb, int width, int height) override
	{
		if (rgb == nullptr)
			return;
		if (!transform || width != sourceWidth || height != sourceHeight)
		{
			resetTransform();
			if (!configure(width, height))
				return;
		}

		ComPtr<IMFMediaBuffer> buffer;
		ComPtr<IMFSample> sample;
		BYTE *pixels = nullptr;
		if (FAILED(MFCreateSample(sample.GetAddressOf()))
				|| FAILED(MFCreateMemoryBuffer(inputBytes, buffer.GetAddressOf()))
				|| FAILED(buffer->Lock(&pixels, nullptr, nullptr)))
			return;
		rgbToNv12(rgb, width, height, pixels, encodedWidth, encodedHeight);
		const LONGLONG timestamp = frameIndex * 10000000 / fps;
		const LONGLONG duration = (frameIndex + 1) * 10000000 / fps - timestamp;
		if (FAILED(buffer->Unlock())
				|| FAILED(buffer->SetCurrentLength(inputBytes))
				|| FAILED(sample->AddBuffer(buffer.Get()))
				|| FAILED(sample->SetSampleTime(timestamp))
				|| FAILED(sample->SetSampleDuration(duration)))
			return;

		const bool requested = forceKeyframe || frameIndex - lastKeyframe >= fps * 2;
		if (requested)
			setCodecUInt32(codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
		HRESULT result = transform->ProcessInput(0, sample.Get(), 0);
		if (result == MF_E_NOTACCEPTING && drainOutput())
			result = transform->ProcessInput(0, sample.Get(), 0);
		if (FAILED(result) || !drainOutput())
		{
			resetTransform();
			return;
		}
		++frameIndex;
	}

private:
	void resetTransform()
	{
		if (transform)
		{
			transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
			transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
		}
		codec.Reset();
		transform.Reset();
		sps.clear();
		pps.clear();
		haveKeyframe = false;
	}

	void readSequenceHeader()
	{
		ComPtr<IMFMediaType> type;
		if (FAILED(transform->GetOutputCurrentType(0, type.GetAddressOf())))
			return;
		UINT32 size = 0;
		if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0)
			return;
		std::vector<u8> header(size);
		if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, &size)))
			forEachAnnexBNal(header.data(), size, [this](const u8 *nal, size_t length) {
				cacheParameterSet(nal, length);
			});
	}

	void cacheParameterSet(const u8 *nal, size_t size)
	{
		switch (nal[0] & 0x1f)
		{
		case 7:
			sps.assign(nal, nal + size);
			break;
		case 8:
			pps.assign(nal, nal + size);
			break;
		}
	}

	bool drainOutput()
	{
		MFT_OUTPUT_STREAM_INFO info{};
		if (FAILED(transform->GetOutputStreamInfo(0, &info)))
			return false;
		for (;;)
		{
			ComPtr<IMFSample> sample;
			if ((info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0)
			{
				ComPtr<IMFMediaBuffer> buffer;
				const DWORD alignment = info.cbAlignment > 0 ? info.cbAlignment - 1 : 0;
				if (FAILED(MFCreateSample(sample.GetAddressOf()))
						|| FAILED(MFCreateAlignedMemoryBuffer(info.cbSize, alignment, buffer.GetAddressOf()))
						|| FAILED(sample->AddBuffer(buffer.Get())))
					return false;
			}

			MFT_OUTPUT_DATA_BUFFER output{};
			output.pSample = sample.Get();
			DWORD status = 0;
			const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
			if (output.pEvents != nullptr)
				output.pEvents->Release();
			if (output.pSample != sample.Get())
				sample.Attach(output.pSample);
			if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
				return true;
			if (FAILED(result))
				return false;
			if (sample && (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) != MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE
					&& !deliverSample(sample.Get()))
				return false;
			if ((output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) == 0)
				return true;
		}
	}

	bool deliverSample(IMFSample *sample)
	{
		ComPtr<IMFMediaBuffer> buffer;
		BYTE *data = nullptr;
		DWORD size = 0;
		if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))
				|| FAILED(buffer->Lock(&data, nullptr, &size)))
			return false;

		std::vector<u8> avcc;
		bool idr = false;
		bool hasPicture = false;
		forEachAnnexBNal(data, size, [&](const u8 *nal, size_t length) {
			const int type = nal[0] & 0x1f;
			if (type == 7 || type == 8)
			{
				cacheParameterSet(nal, length);
				return;
			}
			idr |= type == 5;
			hasPicture |= type == 1 || type == 5;
			appendAvcc(avcc, nal, length);
		});
		if (FAILED(buffer->Unlock()))
			return false;

		if (idr)
		{
			if (sps.empty() || pps.empty())
				readSequenceHeader();
			if (sps.empty() || pps.empty())
			{
				haveKeyframe = false;
				forceKeyframe = true;
				return true;
			}
			std::vector<u8> keyframe;
			keyframe.reserve(8 + sps.size() + pps.size() + avcc.size());
			appendAvcc(keyframe, sps.data(), sps.size());
			appendAvcc(keyframe, pps.data(), pps.size());
			keyframe.insert(keyframe.end(), avcc.begin(), avcc.end());
			haveKeyframe = true;
			forceKeyframe = false;
			lastKeyframe = frameIndex;
			callback(keyframe, true);
		}
		else if (hasPicture && haveKeyframe)
			callback(avcc, false);
		return true;
	}

	MediaFoundation mediaFoundation;
	ComPtr<IMFTransform> transform;
	ComPtr<ICodecAPI> codec;
	const int fps;
	const int bitrateKbps;
	SampleCallback callback;
	std::vector<u8> sps;
	std::vector<u8> pps;
	int sourceWidth = 0;
	int sourceHeight = 0;
	int encodedWidth = 0;
	int encodedHeight = 0;
	DWORD inputBytes = 0;
	LONGLONG frameIndex = 0;
	LONGLONG lastKeyframe = 0;
	bool forceKeyframe = true;
	bool haveKeyframe = false;
};

} // namespace

bool h264EncoderAvailable()
{
	static const bool available = [] {
		MediaFoundation mediaFoundation;
		ComPtr<IMFTransform> transform;
		return mediaFoundation.ready() && SUCCEEDED(createTransform(transform));
	}();
	return available;
}

std::unique_ptr<H264Encoder> H264Encoder::create(int width, int height, int fps, int bitrateKbps,
		SampleCallback callback)
{
	if (fps <= 0 || fps > (std::numeric_limits<int>::max)() / 2
			|| bitrateKbps <= 0 || bitrateKbps > (std::numeric_limits<UINT32>::max)() / 1000 || !callback)
		return nullptr;
	auto encoder = std::make_unique<H264EncoderMf>(fps, bitrateKbps, std::move(callback));
	if (!encoder->configure(width, height))
		return nullptr;
	return encoder;
}

} // namespace remoteplay
