# Portions Copyright 2026 The Hollycast Authors
# Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.22)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(HikaruBusTests LANGUAGES CXX)
    enable_testing()
endif()

if(NOT TARGET hollycast_hikaru_bus)
    add_subdirectory(../../core/hw/hikaru "${CMAKE_CURRENT_BINARY_DIR}/hikaru")
endif()

foreach(component bridge board_memory sh4_cpu sh4_peripherals sh4_dma roms jvs mie machine eeprom93c46 protection)
    add_executable(hikaru_${component}_test ${component}_test.cpp)
    target_link_libraries(hikaru_${component}_test PRIVATE hollycast_hikaru_bus)
    if(MSVC)
        target_compile_options(hikaru_${component}_test PRIVATE /W4)
    else()
        target_compile_options(hikaru_${component}_test PRIVATE -Wall -Wextra -Werror)
        if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # New libc++ SDKs warn about the host's older deployment target.
            # Keep that SDK notice visible; retain errors for source warnings.
            target_compile_options(hikaru_${component}_test PRIVATE -Wno-error=cpp)
        endif()
    endif()
    add_test(NAME hikaru_${component} COMMAND hikaru_${component}_test)
endforeach()

option(HIKARU_NATIVE_GRAPHICS_TESTS "Build Hikaru graphics dispatch and native shader checks" OFF)
if(HIKARU_NATIVE_GRAPHICS_TESTS)
    enable_language(C)
    add_executable(hikaru_graphics_device_test graphics_device_test.cpp
        ../../core/hw/hikaru/graphics/device.cpp
        ../../core/hw/hikaru/valkyrie/graphics_dispatch.c)
    target_include_directories(hikaru_graphics_device_test PRIVATE ../../core
        ../../core/hw/hikaru/valkyrie/src)
    target_compile_features(hikaru_graphics_device_test PRIVATE cxx_std_17 c_std_11)
    target_compile_definitions(hikaru_graphics_device_test PRIVATE GL_SILENCE_DEPRECATION)
    if(APPLE)
        target_link_libraries(hikaru_graphics_device_test PRIVATE "-framework OpenGL")
    else()
        target_include_directories(hikaru_graphics_device_test PRIVATE ../../core/deps/glad/include)
        target_sources(hikaru_graphics_device_test PRIVATE ../../core/deps/glad/src/gl.c)
        if(CMAKE_DL_LIBS)
            target_link_libraries(hikaru_graphics_device_test PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endif()
    add_test(NAME hikaru_graphics_device COMMAND hikaru_graphics_device_test)
    if(APPLE)
        enable_language(OBJCXX)
        foreach(check native_shader metal_backend)
            add_executable(hikaru_${check}_smoke ${check}_smoke.mm
                ../../core/hw/hikaru/graphics/shaders.cpp)
            target_include_directories(hikaru_${check}_smoke PRIVATE ../../core ../../core/deps
                ../../core/deps/nowide/include ../../core/deps/glm)
            target_compile_features(hikaru_${check}_smoke PRIVATE cxx_std_17)
            target_compile_options(hikaru_${check}_smoke PRIVATE
                -fobjc-arc -ffunction-sections -fdata-sections)
            target_link_options(hikaru_${check}_smoke PRIVATE -Wl,-dead_strip)
            target_link_libraries(hikaru_${check}_smoke PRIVATE "-framework Metal" "-framework Foundation")
            add_test(NAME hikaru_${check}_smoke COMMAND hikaru_${check}_smoke)
            set_tests_properties(hikaru_${check}_smoke PROPERTIES SKIP_RETURN_CODE 77)
        endforeach()
        target_sources(hikaru_metal_backend_smoke PRIVATE ../../core/hw/hikaru/graphics/metal.mm)
    endif()
endif()

# The standalone harness runs host-only regressions without a playable emulator.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(../../core/deps/googletest "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
    add_subdirectory(../../core/deps/nowide "${CMAKE_CURRENT_BINARY_DIR}/nowide" EXCLUDE_FROM_ALL)
    add_subdirectory(../../core/deps/soundtouch "${CMAKE_CURRENT_BINARY_DIR}/soundtouch")
    add_executable(hikaru_host_test ../src/HikaruAudioPacerTest.cpp ../src/HikaruAuditTest.cpp ../src/HikaruInputBindingsTest.cpp ../src/HikaruCabinetSettingsTest.cpp
        ../../core/hw/hikaru/audio_pacer.cpp)
    target_include_directories(hikaru_host_test PRIVATE ../../core)
    target_compile_features(hikaru_host_test PRIVATE cxx_std_17)
    target_link_libraries(hikaru_host_test PRIVATE GTest::gtest_main nowide::nowide hollycast_soundtouch hollycast_hikaru_io)
    add_test(NAME hikaru_host COMMAND hikaru_host_test)
endif()
