<!-- Portions Copyright 2026 The Hollycast Authors -->

# Runtime Work Reduction

The change skips desktop VMU display work while VMU windows are disabled, avoids unchanged Google Play updater reads, shares one library-statistics scan per draw, and skips an unnecessary Metal depth-buffer round trip for opaque-only unsorted rendering. The Android updater measurements in the PR used host-side stubs, not an Android device.

[Source pull request #79](https://github.com/nexus382/Hollycast/pull/79)
