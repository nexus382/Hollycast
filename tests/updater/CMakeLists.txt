# Portions Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HollycastUpdaterTests LANGUAGES C CXX)
    enable_testing()
endif()
get_filename_component(UPDATER_ROOT "../.." ABSOLUTE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define GIT_VERSION \"test\"\n#define GIT_HASH \"000000000\"\n#define BUILD_DATE \"2026-11-11\"\n#define HOLLYCAST_UPDATE_CHANNEL \"dev\"\n")
add_executable(update_catalog catalog.cpp "${UPDATER_ROOT}/core/ui/app_update_catalog.cpp")
target_compile_features(update_catalog PRIVATE cxx_std_17)
target_compile_definitions(update_catalog PRIVATE NOMINMAX)
target_include_directories(update_catalog PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${UPDATER_ROOT}/core"
    "${UPDATER_ROOT}/core/deps/json" "${UPDATER_ROOT}/core/deps/nowide/include")
add_test(NAME updater_catalog COMMAND update_catalog)
if(WIN32)
    add_executable(update_parent dummy_application.cpp)
    add_executable(update_payload dummy_application.cpp)
    target_compile_definitions(update_payload PRIVATE UPDATE_PAYLOAD)
    find_package(Python3 COMPONENTS Interpreter)
    if(TARGET HollycastUpdater)
        set(UPDATER_HELPER "$<TARGET_FILE:HollycastUpdater>")
    endif()
    if(Python3_FOUND AND UPDATER_HELPER)
        add_test(NAME updater_windows_install COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_install.py"
            --helper "${UPDATER_HELPER}" --parent "$<TARGET_FILE:update_parent>" --payload "$<TARGET_FILE:update_payload>")
        set_tests_properties(updater_windows_install PROPERTIES TIMEOUT 240)
    endif()
endif()

# This exercises the desktop frame loop without Android's Java/Play bridge.
if(ANDROID OR IOS OR NINTENDO_SWITCH)
    return()
endif()
if(NOT TARGET freetype)
    foreach(component ZLIB BZIP2 PNG HARFBUZZ BROTLI)
        set(FT_DISABLE_${component} ON CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${UPDATER_ROOT}/core/deps/freetype" updater-freetype EXCLUDE_FROM_ALL)
endif()
add_executable(update_startup_ui startup_ui.cpp "${UPDATER_ROOT}/core/ui/app_updater.cpp"
    "${UPDATER_ROOT}/core/deps/imgui/imgui.cpp" "${UPDATER_ROOT}/core/deps/imgui/imgui_draw.cpp"
    "${UPDATER_ROOT}/core/deps/imgui/imgui_widgets.cpp" "${UPDATER_ROOT}/core/deps/imgui/imgui_tables.cpp"
    "${UPDATER_ROOT}/core/deps/imgui/imgui_cjk.cpp"
    "${UPDATER_ROOT}/core/deps/imgui/misc/freetype/imgui_freetype.cpp"
    "${UPDATER_ROOT}/core/deps/imgui/misc/freetype/imgui_text_shaping.cpp")
target_link_libraries(update_startup_ui PRIVATE freetype)
target_compile_features(update_startup_ui PRIVATE cxx_std_17)
target_compile_definitions(update_startup_ui PRIVATE NOMINMAX IMGUI_DEFINE_MATH_OPERATORS)
target_include_directories(update_startup_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${UPDATER_ROOT}/core"
    "${UPDATER_ROOT}/core/deps" "${UPDATER_ROOT}/core/deps/json"
    "${UPDATER_ROOT}/core/deps/imgui" "${UPDATER_ROOT}/core/deps/nowide/include")
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(update_startup_ui PRIVATE Threads::Threads)
endif()
add_test(NAME updater_startup_popup COMMAND update_startup_ui startup)
add_test(NAME updater_deferred_popup COMMAND update_startup_ui defer)
add_test(NAME updater_manual_without_startup COMMAND update_startup_ui disabled)

set_tests_properties(updater_startup_popup updater_deferred_popup updater_manual_without_startup PROPERTIES TIMEOUT 15)
