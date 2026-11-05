# SPDX-License-Identifier: GPL-3.0-or-later
# Portions Copyright 2026 The Hollycast Authors
# Optional experimental OpenGL/OpenGL ES Hikaru backend. The host owns GL context.
set(_hikaru_vendor_sources
    engine.c graphics_dispatch.c
    src/vk/core.c src/vk/vector.c src/vk/state.c src/vk/buffer.c
    src/vk/mmap.c src/vk/region.c src/vk/machine.c src/vk/renderer.c
    src/cpu/sh/sh4.c
    src/mach/hikaru/hikaru.c src/mach/hikaru/hikaru-mscomm.c
    src/mach/hikaru/hikaru-mie.c src/mach/hikaru/hikaru-memctl.c
    src/mach/hikaru/hikaru-renderer.c src/mach/hikaru/hikaru-gpu.c
    src/mach/hikaru/hikaru-gpu-cp.c src/mach/hikaru/hikaru-gpu-private.c
    src/mach/hikaru/hikaru-aica.c)
option(HIKARU_VENDOR_OPTIMIZE "Optimize the experimental Hikaru core" ON)
function(add_hikaru_vendor_library target)
    add_library(${target} STATIC ${_hikaru_vendor_sources})
    target_compile_features(${target} PRIVATE c_std_11)
    # This optional backend spends much of its time in the SH-4 interpreter.
    # Optimize only its C sources in optimized builds; keep debug and sanitizer
    # builds at their requested optimization level and do not enable fast-math.
    set(_hikaru_c_flags "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE} ${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    if(HIKARU_VENDOR_OPTIMIZE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
       NOT ASAN AND NOT _hikaru_c_flags MATCHES "-fsanitize")
        target_compile_options(${target} PRIVATE
            "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O3>")
    endif()
    unset(_hikaru_c_flags)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../../../deps/glad/include")
    if(APPLE)
        target_compile_definitions(${target} PRIVATE GL_SILENCE_DEPRECATION)
    endif()
    if(UNIX AND NOT APPLE)
        # C test executables do not inherit libm from the C++ host; the
        # renderer uses lroundf/sincosf/sqrtf from the vendor sources.
        target_link_libraries(${target} PUBLIC m)
    endif()
    if(APPLE)
        target_link_libraries(${target} PUBLIC "-framework OpenGL")
    endif()
endfunction()
add_hikaru_vendor_library(hollycast_hikaru_valkyrie)

option(HIKARU_VENDOR_BUILD_SMOKE "Build the synthetic hidden-window GL smoke test" OFF)
if(HIKARU_VENDOR_BUILD_SMOKE AND NOT ANDROID AND NOT IOS AND NOT LIBRETRO)
    # Test-only switches sit in instruction and memory hot paths. Never link
    # that instrumented copy into the playable production executable.
    add_hikaru_vendor_library(hollycast_hikaru_valkyrie_test)
    target_compile_definitions(hollycast_hikaru_valkyrie_test PRIVATE HIKARU_VENDOR_TESTING)
    add_executable(hikaru_valkyrie_sound_bus_timing tests/sound_bus_timing.c)
    target_include_directories(hikaru_valkyrie_sound_bus_timing PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_sound_bus_timing PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_sound_bus_timing COMMAND hikaru_valkyrie_sound_bus_timing)
    add_executable(hikaru_valkyrie_gpu_irq tests/gpu_irq.c)
    target_include_directories(hikaru_valkyrie_gpu_irq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_gpu_irq PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_gpu_irq COMMAND hikaru_valkyrie_gpu_irq)
    add_executable(hikaru_valkyrie_tmu_timing tests/tmu_timing.c)
    target_include_directories(hikaru_valkyrie_tmu_timing PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_tmu_timing PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_tmu_timing COMMAND hikaru_valkyrie_tmu_timing)
    add_executable(hikaru_valkyrie_cpu_clock tests/cpu_clock.c)
    target_include_directories(hikaru_valkyrie_cpu_clock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_cpu_clock PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_cpu_clock COMMAND hikaru_valkyrie_cpu_clock)
    add_executable(hikaru_valkyrie_texture_bounds tests/texture_bounds.c)
    target_include_directories(hikaru_valkyrie_texture_bounds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_texture_bounds PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_texture_bounds COMMAND hikaru_valkyrie_texture_bounds)

    add_executable(hikaru_valkyrie_framebuffer_snapshot tests/framebuffer_snapshot.c)
    target_include_directories(hikaru_valkyrie_framebuffer_snapshot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_framebuffer_snapshot PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_framebuffer_snapshot COMMAND hikaru_valkyrie_framebuffer_snapshot)

    add_executable(hikaru_valkyrie_vertex_storage tests/vertex_storage.c)
    target_link_libraries(hikaru_valkyrie_vertex_storage PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_vertex_storage COMMAND hikaru_valkyrie_vertex_storage)
    add_executable(hikaru_valkyrie_normal_matrix tests/normal_matrix.c)
    target_include_directories(hikaru_valkyrie_normal_matrix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_normal_matrix PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_normal_matrix COMMAND hikaru_valkyrie_normal_matrix)
    add_executable(hikaru_valkyrie_mmap_fetch tests/mmap_fetch.c)
    target_include_directories(hikaru_valkyrie_mmap_fetch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_mmap_fetch PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_mmap_fetch COMMAND hikaru_valkyrie_mmap_fetch)

    add_executable(hikaru_valkyrie_mmap_data tests/mmap_data.c)
    target_include_directories(hikaru_valkyrie_mmap_data PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_mmap_data PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_mmap_data COMMAND hikaru_valkyrie_mmap_data)

    add_executable(hikaru_valkyrie_mmap_write tests/mmap_write.c)
    target_include_directories(hikaru_valkyrie_mmap_write PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_mmap_write PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_mmap_write COMMAND hikaru_valkyrie_mmap_write)

    add_executable(hikaru_valkyrie_cached_writes tests/cached_writes.c)
    target_include_directories(hikaru_valkyrie_cached_writes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_cached_writes PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_cached_writes COMMAND hikaru_valkyrie_cached_writes)

    add_executable(hikaru_valkyrie_cached_reads tests/cached_reads.c)
    target_include_directories(hikaru_valkyrie_cached_reads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_cached_reads PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_cached_reads COMMAND hikaru_valkyrie_cached_reads)

    add_executable(hikaru_valkyrie_dispatch tests/dispatch.c)
    target_include_directories(hikaru_valkyrie_dispatch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_dispatch PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_dispatch COMMAND hikaru_valkyrie_dispatch)

    add_executable(hikaru_valkyrie_poll_loop tests/poll_loop.c)
    target_include_directories(hikaru_valkyrie_poll_loop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_poll_loop PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_poll_loop COMMAND hikaru_valkyrie_poll_loop)

    add_executable(hikaru_valkyrie_queue_wait tests/queue_wait.c)
    target_include_directories(hikaru_valkyrie_queue_wait PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_queue_wait PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_queue_wait COMMAND hikaru_valkyrie_queue_wait)

    add_executable(hikaru_valkyrie_slave_wait tests/slave_wait.c)
    target_include_directories(hikaru_valkyrie_slave_wait PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_slave_wait PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_slave_wait COMMAND hikaru_valkyrie_slave_wait)
    add_executable(hikaru_valkyrie_planet_slave_wait tests/planet_slave_wait.c)
    target_include_directories(hikaru_valkyrie_planet_slave_wait PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_planet_slave_wait PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_planet_slave_wait COMMAND hikaru_valkyrie_planet_slave_wait)
    add_executable(hikaru_valkyrie_master_wait tests/master_wait.c)
    target_include_directories(hikaru_valkyrie_master_wait PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_master_wait PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_master_wait COMMAND hikaru_valkyrie_master_wait)
    add_executable(hikaru_valkyrie_double_precision tests/double_precision.c)
    target_include_directories(hikaru_valkyrie_double_precision PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_double_precision PRIVATE hollycast_hikaru_valkyrie_test)
    add_test(NAME hikaru_valkyrie_double_precision COMMAND hikaru_valkyrie_double_precision)
    # The bundled SDL is already a target; only look for a system SDL2
    # when it is not. Linking below uses the SDL2::SDL2 target either way.
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 CONFIG REQUIRED)
    endif()
    add_executable(hikaru_valkyrie_engine_games tests/engine_games.c)
    target_include_directories(hikaru_valkyrie_engine_games PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_engine_games PRIVATE hollycast_hikaru_valkyrie_test SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_engine_games COMMAND hikaru_valkyrie_engine_games)
    add_test(NAME hikaru_valkyrie_engine_games_profile COMMAND hikaru_valkyrie_engine_games --profile)
    set_tests_properties(hikaru_valkyrie_engine_games hikaru_valkyrie_engine_games_profile PROPERTIES RUN_SERIAL TRUE)

    add_executable(hikaru_valkyrie_spot_pixels tests/spot_pixels.c)
    target_include_directories(hikaru_valkyrie_spot_pixels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_spot_pixels PRIVATE hollycast_hikaru_valkyrie_test SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_spot_pixels COMMAND hikaru_valkyrie_spot_pixels)
    set_tests_properties(hikaru_valkyrie_spot_pixels PROPERTIES RUN_SERIAL TRUE)

    add_executable(hikaru_valkyrie_material_pixels tests/material_pixels.c)
    target_include_directories(hikaru_valkyrie_material_pixels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_material_pixels PRIVATE hollycast_hikaru_valkyrie_test SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_material_pixels COMMAND hikaru_valkyrie_material_pixels)
    set_tests_properties(hikaru_valkyrie_material_pixels PROPERTIES RUN_SERIAL TRUE)

    add_executable(hikaru_valkyrie_gl_smoke tests/gl_smoke.c)
    target_include_directories(hikaru_valkyrie_gl_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_gl_smoke PRIVATE hollycast_hikaru_valkyrie_test SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_gl_smoke COMMAND hikaru_valkyrie_gl_smoke)
    add_executable(hikaru_valkyrie_sorting_pixels tests/sorting_pixels.c)
    target_include_directories(hikaru_valkyrie_sorting_pixels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(hikaru_valkyrie_sorting_pixels PRIVATE hollycast_hikaru_valkyrie_test SDL2::SDL2)
    add_test(NAME hikaru_valkyrie_sorting_pixels COMMAND hikaru_valkyrie_sorting_pixels)
    set_tests_properties(hikaru_valkyrie_sorting_pixels PROPERTIES RUN_SERIAL TRUE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME hikaru_valkyrie_engine_games_gles COMMAND hikaru_valkyrie_engine_games --gles)
        add_test(NAME hikaru_valkyrie_spot_pixels_gles COMMAND hikaru_valkyrie_spot_pixels --gles)
        add_test(NAME hikaru_valkyrie_sorting_pixels_gles COMMAND hikaru_valkyrie_sorting_pixels --gles)
        set_tests_properties(hikaru_valkyrie_engine_games_gles hikaru_valkyrie_spot_pixels_gles hikaru_valkyrie_sorting_pixels_gles PROPERTIES RUN_SERIAL TRUE)
    endif()
    if(NOT APPLE)
        # glad is created by the root build after this directory; CMake
        # resolves the target at generate time. It supplies the GL
        # entry points the Apple builds get from the OpenGL framework.
        foreach(_hikaru_test sound_bus_timing tmu_timing cpu_clock gpu_irq texture_bounds framebuffer_snapshot vertex_storage normal_matrix mmap_fetch mmap_data mmap_write cached_writes cached_reads dispatch double_precision poll_loop queue_wait slave_wait planet_slave_wait master_wait engine_games spot_pixels gl_smoke sorting_pixels material_pixels)
            # Vendor headers use C11 (_Noreturn, _Thread_local); MSVC
            # defaults these test targets to an older C mode.
            target_compile_features(hikaru_valkyrie_${_hikaru_test} PRIVATE c_std_11)
            target_link_libraries(hikaru_valkyrie_${_hikaru_test} PRIVATE glad)
        endforeach()
        # Bundled SDL provides SDL2main separately; these tests own main.
        foreach(_hikaru_test engine_games spot_pixels gl_smoke sorting_pixels material_pixels)
            target_compile_definitions(hikaru_valkyrie_${_hikaru_test} PRIVATE SDL_MAIN_HANDLED)
        endforeach()
        unset(_hikaru_test)
    endif()
endif()

unset(_hikaru_vendor_sources)
