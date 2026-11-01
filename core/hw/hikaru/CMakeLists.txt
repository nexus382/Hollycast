# Copyright 2026 The Hollycast Authors
# Kept independent from the Dreamcast/Naomi core during machine bring-up.
add_library(hollycast_hikaru_bus STATIC bridge.cpp board_memory.cpp roms.cpp
    sh4_cpu.cpp sh4_peripherals.cpp sh4_dma.cpp jvs.cpp mie.cpp machine.cpp eeprom93c46.cpp
    protection.cpp crypto3155881.cpp)
target_compile_features(hollycast_hikaru_bus PUBLIC cxx_std_17)
target_include_directories(hollycast_hikaru_bus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(NOT MSVC)
    # The SH4 FPU models guest rounding modes and preserves the caller's fenv.
    set_source_files_properties(sh4_cpu.cpp PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-frounding-math;-ffp-contract=off")
endif()
