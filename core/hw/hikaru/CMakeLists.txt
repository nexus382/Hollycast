# Portions Copyright 2026 The Hollycast Authors
# Copyright 2026 The Hollycast Authors
# The player uses these board devices alongside the vendored playback CPUs.
add_library(hollycast_hikaru_io STATIC roms.cpp jvs.cpp mie.cpp eeprom93c46.cpp
    protection.cpp crypto3155881.cpp input_bindings.cpp cabinet_settings.cpp)
target_compile_features(hollycast_hikaru_io PUBLIC cxx_std_17)
target_include_directories(hollycast_hikaru_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Keep the independent CPU/bus bring-up model for its regressions without
# compiling or linking it into normal player builds.
add_library(hollycast_hikaru_bus STATIC EXCLUDE_FROM_ALL bridge.cpp board_memory.cpp
    sh4_cpu.cpp sh4_peripherals.cpp sh4_dma.cpp machine.cpp)
target_link_libraries(hollycast_hikaru_bus PUBLIC hollycast_hikaru_io)
target_compile_features(hollycast_hikaru_bus PUBLIC cxx_std_17)
target_include_directories(hollycast_hikaru_bus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(NOT MSVC)
    # The SH4 FPU models guest rounding modes and preserves the caller's fenv.
    set_source_files_properties(sh4_cpu.cpp PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-frounding-math;-ffp-contract=off")
endif()
