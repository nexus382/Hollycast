// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <cstddef>
#include <cstring>
#include <mutex>

// SPG timing is produced by the emulation thread and consumed by retro_run.
class PendingFps
{
public:
	void publish(double fps)
	{
		std::lock_guard<std::mutex> lock(mutex);
		pending = fps;
	}
	double consume()
	{
		std::lock_guard<std::mutex> lock(mutex);
		const double fps = pending;
		pending = 0.0;
		return fps;
	}
	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		pending = 0.0;
	}

private:
	std::mutex mutex;
	double pending = 0.0;
};

template<typename Callback>
bool updatePendingFps(PendingFps& pending, double& currentFps, Callback&& onChange)
{
	const double fps = pending.consume();
	if (fps <= 0.0 || fps >= 100.0 || fps == currentFps)
		return false;
	currentFps = fps;
	onChange(fps);
	return true;
}

inline void resetPendingFps(PendingFps& pending, double& currentFps)
{
	pending.clear();
	currentFps = 0.0;
}

inline void clearSerializePadding(void *data, size_t bufferSize, size_t serializedSize)
{
	if (data != nullptr && serializedSize < bufferSize)
		std::memset(static_cast<unsigned char *>(data) + serializedSize, 0,
				bufferSize - serializedSize);
}
