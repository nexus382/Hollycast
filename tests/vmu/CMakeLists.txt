# Portions Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.20)
project(VmuRegressionTests LANGUAGES CXX)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(capture_source "${CMAKE_CURRENT_BINARY_DIR}/capture.cpp")
add_custom_command(OUTPUT "${capture_source}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_capture_test.py" "${capture_source}"
    DEPENDS generate_capture_test.py ../../core/sdl/sdl.cpp ../../core/ui/gui.cpp ../../core/windows/vmu_window.cpp
        ../../core/input/mouse.cpp ../../core/windows/rawinput.cpp
    VERBATIM)
add_custom_target(vmu_capture_source DEPENDS "${capture_source}")
set(publish_source "${CMAKE_CURRENT_BINARY_DIR}/publish.cpp")
add_custom_command(OUTPUT "${publish_source}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_publish_test.py" "${publish_source}"
    DEPENDS generate_publish_test.py ../../core/rend/osd.cpp ../../core/rend/osd.h ../../core/ui/gui.cpp
    VERBATIM)
add_executable(vmu_publication "${publish_source}")
target_compile_features(vmu_publication PRIVATE cxx_std_17)
add_test(NAME vmu_publication COMMAND vmu_publication)
foreach(platform windows libretro uwp other)
    set(target vmu_capture_${platform})
    add_executable(${target} "${capture_source}")
    add_dependencies(${target} vmu_capture_source)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(platform STREQUAL "other")
        target_compile_definitions(${target} PRIVATE VMU_TEST_OTHER)
    elseif(platform STREQUAL "libretro")
        target_compile_definitions(${target} PRIVATE LIBRETRO)
    elseif(platform STREQUAL "uwp")
        target_compile_definitions(${target} PRIVATE TARGET_UWP)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endforeach()

find_package(Threads REQUIRED)
add_executable(vmu_touch_layout touch_layout.cpp)
target_compile_features(vmu_touch_layout PRIVATE cxx_std_17)
target_include_directories(vmu_touch_layout PRIVATE ../../core)
target_link_libraries(vmu_touch_layout PRIVATE Threads::Threads)
add_test(NAME vmu_touch_layout COMMAND vmu_touch_layout)

find_package(Java 11 COMPONENTS Runtime Development)
if(Java_FOUND)
    add_test(NAME vmu_android_touch
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/android_touch_test.py"
            "${CMAKE_CURRENT_BINARY_DIR}/android-touch" "${Java_JAVAC_EXECUTABLE}" "${Java_JAVA_EXECUTABLE}")
endif()

if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(SDL2 2.0.18 REQUIRED)
    add_executable(vmu_native_sdl native_sdl.cpp)
    target_compile_features(vmu_native_sdl PRIVATE cxx_std_17)
    target_compile_definitions(vmu_native_sdl PRIVATE USE_SDL)
    target_include_directories(vmu_native_sdl PRIVATE fixtures ../../core)
    target_link_libraries(vmu_native_sdl PRIVATE SDL2::SDL2 Threads::Threads)
    if(APPLE)
        enable_language(OBJCXX)
        target_sources(vmu_native_sdl PRIVATE ../../core/sdl/vmu_window_macos.mm)
        target_link_libraries(vmu_native_sdl PRIVATE "-framework AppKit")
    endif()
    add_test(NAME vmu_native_sdl COMMAND vmu_native_sdl)
    set_tests_properties(vmu_native_sdl PROPERTIES LABELS interactive RUN_SERIAL TRUE TIMEOUT 60)
endif()

if(WIN32)
    add_executable(vmu_native_windows native_windows.cpp)
    target_compile_features(vmu_native_windows PRIVATE cxx_std_17)
    target_include_directories(vmu_native_windows PRIVATE fixtures ../../core)
    target_link_libraries(vmu_native_windows PRIVATE user32 gdi32)
    add_test(NAME vmu_native_windows COMMAND vmu_native_windows)
    set_tests_properties(vmu_native_windows PROPERTIES LABELS interactive RUN_SERIAL TRUE TIMEOUT 120)
endif()
