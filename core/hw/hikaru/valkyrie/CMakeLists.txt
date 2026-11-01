# SPDX-License-Identifier: GPL-3.0-or-later
# Optional experimental desktop OpenGL Hikaru backend. The host owns GL context.
add_library(hollycast_hikaru_valkyrie STATIC
    engine.c
    src/vk/core.c src/vk/vector.c src/vk/state.c src/vk/buffer.c
    src/vk/mmap.c src/vk/region.c src/vk/machine.c src/vk/renderer.c
    src/cpu/sh/sh4.c
    src/mach/hikaru/hikaru.c src/mach/hikaru/hikaru-mscomm.c
    src/mach/hikaru/hikaru-mie.c src/mach/hikaru/hikaru-memctl.c
    src/mach/hikaru/hikaru-renderer.c src/mach/hikaru/hikaru-gpu.c
    src/mach/hikaru/hikaru-gpu-cp.c src/mach/hikaru/hikaru-gpu-private.c
    src/mach/hikaru/hikaru-aica.c)
target_compile_features(hollycast_hikaru_valkyrie PRIVATE c_std_11)
# This optional backend spends much of its time in the SH-4 interpreter.
# Optimize only its C sources in optimized builds; keep debug and sanitizer
# builds at their requested optimization level and do not enable fast-math.
option(HIKARU_VENDOR_OPTIMIZE "Optimize the experimental Hikaru core" ON)
set(_hikaru_c_flags "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE} ${CMAKE_C_FLAGS_RELWITHDEBINFO}")
if(HIKARU_VENDOR_OPTIMIZE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
   NOT ASAN AND NOT _hikaru_c_flags MATCHES "-fsanitize")
    target_compile_options(hollycast_hikaru_valkyrie PRIVATE
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O3>")
endif()
unset(_hikaru_c_flags)
target_include_directories(hollycast_hikaru_valkyrie PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/glad/include")
if(APPLE)
    target_compile_definitions(hollycast_hikaru_valkyrie PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(hollycast_hikaru_valkyrie PUBLIC "-framework OpenGL")
endif()

option(HIKARU_VENDOR_BUILD_SMOKE "Build the synthetic hidden-window GL smoke test" OFF)
if(HIKARU_VENDOR_BUILD_SMOKE AND APPLE)
    target_compile_definitions(hollycast_hikaru_valkyrie PRIVATE HIKARU_VENDOR_TESTING)
    add_executable(hikaru_valkyrie_sound_bus_timing tests/sound_bus_timing.c)
    target_include_directories(hikaru_valkyrie_sound_bus_timing PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_sound_bus_timing PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_sound_bus_timing COMMAND hikaru_valkyrie_sound_bus_timing)
    add_executable(hikaru_valkyrie_cpu_clock tests/cpu_clock.c)
    target_include_directories(hikaru_valkyrie_cpu_clock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_cpu_clock PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_cpu_clock COMMAND hikaru_valkyrie_cpu_clock)
    add_executable(hikaru_valkyrie_texture_bounds tests/texture_bounds.c)
    target_include_directories(hikaru_valkyrie_texture_bounds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_texture_bounds PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_texture_bounds COMMAND hikaru_valkyrie_texture_bounds)
    add_executable(hikaru_valkyrie_normal_matrix tests/normal_matrix.c)
    target_include_directories(hikaru_valkyrie_normal_matrix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_normal_matrix PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_normal_matrix COMMAND hikaru_valkyrie_normal_matrix)
    add_executable(hikaru_valkyrie_mmap_fetch tests/mmap_fetch.c)
    target_include_directories(hikaru_valkyrie_mmap_fetch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_mmap_fetch PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_mmap_fetch COMMAND hikaru_valkyrie_mmap_fetch)

    add_executable(hikaru_valkyrie_mmap_data tests/mmap_data.c)
    target_include_directories(hikaru_valkyrie_mmap_data PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_mmap_data PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_mmap_data COMMAND hikaru_valkyrie_mmap_data)

    add_executable(hikaru_valkyrie_mmap_write tests/mmap_write.c)
    target_include_directories(hikaru_valkyrie_mmap_write PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_mmap_write PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_mmap_write COMMAND hikaru_valkyrie_mmap_write)

    add_executable(hikaru_valkyrie_cached_writes tests/cached_writes.c)
    target_include_directories(hikaru_valkyrie_cached_writes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_cached_writes PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_cached_writes COMMAND hikaru_valkyrie_cached_writes)

    add_executable(hikaru_valkyrie_cached_reads tests/cached_reads.c)
    target_include_directories(hikaru_valkyrie_cached_reads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_cached_reads PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_cached_reads COMMAND hikaru_valkyrie_cached_reads)

    add_executable(hikaru_valkyrie_dispatch tests/dispatch.c)
    target_include_directories(hikaru_valkyrie_dispatch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_dispatch PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_dispatch COMMAND hikaru_valkyrie_dispatch)

    add_executable(hikaru_valkyrie_poll_loop tests/poll_loop.c)
    target_include_directories(hikaru_valkyrie_poll_loop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_poll_loop PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_poll_loop COMMAND hikaru_valkyrie_poll_loop)

    add_executable(hikaru_valkyrie_slave_wait tests/slave_wait.c)
    target_include_directories(hikaru_valkyrie_slave_wait PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_slave_wait PRIVATE hollycast_hikaru_valkyrie)
    add_test(NAME hikaru_valkyrie_slave_wait COMMAND hikaru_valkyrie_slave_wait)
    find_package(SDL2 CONFIG REQUIRED)
    add_executable(hikaru_valkyrie_engine_games tests/engine_games.c)
    target_include_directories(hikaru_valkyrie_engine_games PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_engine_games PRIVATE hollycast_hikaru_valkyrie SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_engine_games COMMAND hikaru_valkyrie_engine_games)
    set_tests_properties(hikaru_valkyrie_engine_games PROPERTIES RUN_SERIAL TRUE)

    add_executable(hikaru_valkyrie_spot_pixels tests/spot_pixels.c)
    target_include_directories(hikaru_valkyrie_spot_pixels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_spot_pixels PRIVATE hollycast_hikaru_valkyrie SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_spot_pixels COMMAND hikaru_valkyrie_spot_pixels)
    set_tests_properties(hikaru_valkyrie_spot_pixels PROPERTIES RUN_SERIAL TRUE)

    add_executable(hikaru_valkyrie_gl_smoke tests/gl_smoke.c)
    target_include_directories(hikaru_valkyrie_gl_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_gl_smoke PRIVATE hollycast_hikaru_valkyrie SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_gl_smoke COMMAND hikaru_valkyrie_gl_smoke)
endif()
