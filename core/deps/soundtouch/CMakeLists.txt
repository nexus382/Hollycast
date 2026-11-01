# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 The Hollycast Authors
add_library(hollycast_soundtouch STATIC
    source/AAFilter.cpp source/FIFOSampleBuffer.cpp source/FIRFilter.cpp
    source/InterpolateCubic.cpp source/InterpolateLinear.cpp source/InterpolateShannon.cpp
    source/RateTransposer.cpp source/SoundTouch.cpp source/TDStretch.cpp
    source/cpu_detect_x86.cpp source/mmx_optimized.cpp source/sse_optimized.cpp)
target_compile_features(hollycast_soundtouch PUBLIC cxx_std_17)
target_include_directories(hollycast_soundtouch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(hollycast_soundtouch PRIVATE SOUNDTOUCH_FLOAT_SAMPLES=1)
