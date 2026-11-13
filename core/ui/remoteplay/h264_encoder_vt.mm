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
// VideoToolbox H.264 encoder (macOS/iOS). Low-latency settings: real-time,
// no frame reordering (no B-frames), periodic keyframes.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include "h264_encoder.h"

#include <mutex>

namespace remoteplay {
namespace {

class VtEncoder final : public H264Encoder
{
public:
	VtEncoder(int width, int height, int fps, int bitrateKbps, SampleCallback callback)
		: fps(fps), bitrateKbps(bitrateKbps), callback(std::move(callback))
	{
		createSession(width, height);
	}

	~VtEncoder() override
	{
		destroySession();
	}

	void encodeRgb(const u8 *rgb, int width, int height) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (session == nullptr || width != frameWidth || height != frameHeight)
		{
			destroySession();
			createSession(width, height);
		}
		if (session == nullptr || rgb == nullptr)
			return;
		CVPixelBufferRef pb = nullptr;
		if (CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_32BGRA, nullptr, &pb) != kCVReturnSuccess)
			return;
		CVPixelBufferLockBaseAddress(pb, 0);
		u8 *dst = (u8 *)CVPixelBufferGetBaseAddress(pb);
		size_t rowBytes = CVPixelBufferGetBytesPerRow(pb);
		for (int y = 0; y < height; y++)
		{
			const u8 *src = rgb + (size_t)y * width * 3;
			u8 *dstRow = dst + (size_t)y * rowBytes;
			for (int x = 0; x < width; x++)
			{
				dstRow[x * 4 + 0] = src[x * 3 + 2];
				dstRow[x * 4 + 1] = src[x * 3 + 1];
				dstRow[x * 4 + 2] = src[x * 3 + 0];
				dstRow[x * 4 + 3] = 255;
			}
		}
		CVPixelBufferUnlockBaseAddress(pb, 0);
		CMTime pts = CMTimeMake(frameIndex, fps > 0 ? fps : 30);
		CFDictionaryRef frameProps = nullptr;
		if (forceKeyframe)
		{
			forceKeyframe = false;
			const void *keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
			const void *values[] = { kCFBooleanTrue };
			frameProps = CFDictionaryCreate(nullptr, keys, values, 1,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		}
		VTCompressionSessionEncodeFrame(session, pb, pts, CMTimeMake(1, fps > 0 ? fps : 30),
			frameProps, nullptr, nullptr);
		if (frameProps != nullptr)
			CFRelease(frameProps);
		CFRelease(pb);
		frameIndex++;
	}

	void requestKeyframe() override
	{
		std::lock_guard<std::mutex> lock(mutex);
		forceKeyframe = true;
	}

private:
	void createSession(int width, int height)
	{
		frameWidth = width;
		frameHeight = height;
		if (VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264,
				nullptr, nullptr, nullptr, outputCallback, this, &session) != noErr)
		{
			session = nullptr;
			return;
		}
		VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate,
			(__bridge CFTypeRef)@(fps > 0 ? fps : 30));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, (__bridge CFTypeRef)@1);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
			kVTProfileLevel_H264_Baseline_AutoLevel);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
			(__bridge CFTypeRef)@(fps > 0 ? fps : 30));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate,
			(__bridge CFTypeRef)@(bitrateKbps * 1000));
		VTCompressionSessionPrepareToEncodeFrames(session);
	}

	void destroySession()
	{
		if (session != nullptr)
		{
			VTCompressionSessionInvalidate(session);
			CFRelease(session);
			session = nullptr;
		}
	}

	static void appendParameterSet(std::vector<u8>& out, const uint8_t *data, size_t size)
	{
		out.push_back((u8)(size >> 24));
		out.push_back((u8)(size >> 16));
		out.push_back((u8)(size >> 8));
		out.push_back((u8)size);
		out.insert(out.end(), data, data + size);
	}

	static void outputCallback(void *refCon, void *, OSStatus status, VTEncodeInfoFlags,
			CMSampleBufferRef sampleBuffer)
	{
		auto *self = (VtEncoder *)refCon;
		if (status != noErr || sampleBuffer == nullptr || self->callback == nullptr)
			return;
		bool keyframe = true;
		CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
		if (attachments != nullptr && CFArrayGetCount(attachments) > 0)
		{
			auto dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
			CFTypeRef notSync = CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
			keyframe = notSync != kCFBooleanTrue;
		}
		std::vector<u8> sample;
		if (keyframe)
		{
			CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
			const uint8_t *ps = nullptr;
			size_t psSize = 0, count = 0;
			if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &ps, &psSize, &count, nullptr) == noErr)
				appendParameterSet(sample, ps, psSize);
			if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 1, &ps, &psSize, &count, nullptr) == noErr)
				appendParameterSet(sample, ps, psSize);
		}
		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
		size_t length = CMBlockBufferGetDataLength(block);
		size_t offset = sample.size();
		sample.resize(offset + length);
		CMBlockBufferCopyDataBytes(block, 0, length, sample.data() + offset);
		self->callback(sample, keyframe);
	}

	VTCompressionSessionRef session = nullptr;
	std::mutex mutex;
	int fps;
	int bitrateKbps;
	int frameWidth = 0;
	int frameHeight = 0;
	int64_t frameIndex = 0;
	bool forceKeyframe = true;
	SampleCallback callback;
};

} // namespace

std::unique_ptr<H264Encoder> H264Encoder::create(int width, int height, int fps, int bitrateKbps,
		SampleCallback callback)
{
	auto encoder = std::make_unique<VtEncoder>(width, height, fps, bitrateKbps, std::move(callback));
	return encoder;
}

bool h264EncoderAvailable() { return true; }

} // namespace remoteplay
