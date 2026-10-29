// Portions Copyright 2026 The Hollycast Authors
#pragma once

namespace sample_filter
{
// Stored IDs are stable. Spline36 (14) was retired; 19 is nearest-neighbor.
// Canonical limits keep sampler decisions, shader constants and cache keys identical.
struct Settings
{
	int mode = 0;
	int maxSize = 0;
	unsigned sizeBucket = 0;

	constexpr Settings(int requestedMode, int requestedSize)
	{
		if (requestedMode < 1 || requestedMode > 19 || requestedMode == 14 || requestedSize < 8)
			return;
		mode = requestedMode;
		maxSize = 8;
		sizeBucket = 1;
		while (maxSize < 1024 && maxSize * 2 <= requestedSize)
		{
			maxSize *= 2;
			++sizeBucket;
		}
	}

	constexpr bool appliesTo(unsigned uploadedWidth) const
	{
		return mode != 0 && uploadedWidth != 0
				&& (maxSize == 1024 || uploadedWidth <= static_cast<unsigned>(maxSize));
	}

	// Nine non-overlapping bits: five for mode, four for the size bucket.
	constexpr unsigned key() const { return static_cast<unsigned>(mode) | (sizeBucket << 5); }
};
}
