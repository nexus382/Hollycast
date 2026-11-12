# Portions Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HollycastUpdaterTests LANGUAGES C CXX)
    enable_testing()
endif()
get_filename_component(UPDATER_ROOT "../.." ABSOLUTE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define GIT_VERSION \"test\"\n#define GIT_HASH \"000000000\"\n#define BUILD_DATE \"2026-11-11\"\n#define HOLLYCAST_UPDATE_CHANNEL \"dev\"\n")
add_executable(update_catalog catalog.cpp "${UPDATER_ROOT}/core/ui/app_update_catalog.cpp")
add_executable(update_catalog_system catalog.cpp "${UPDATER_ROOT}/core/ui/app_update_catalog.cpp")
target_compile_definitions(update_catalog_system PRIVATE TARGET_IPHONE)
foreach(target update_catalog update_catalog_system)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE NOMINMAX)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${UPDATER_ROOT}/core"
        "${UPDATER_ROOT}/core/deps/json" "${UPDATER_ROOT}/core/deps/nowide/include")
endforeach()
add_test(NAME updater_catalog COMMAND update_catalog)
add_test(NAME updater_catalog_system COMMAND update_catalog_system)
add_executable(update_cleanup cleanup.cpp "${UPDATER_ROOT}/core/ui/app_update_cleanup.cpp")
target_compile_features(update_cleanup PRIVATE cxx_std_17)
target_include_directories(update_cleanup PRIVATE "${UPDATER_ROOT}/core")
add_test(NAME updater_cleanup COMMAND update_cleanup "${CMAKE_CURRENT_BINARY_DIR}")
if(WIN32)
    if(NOT TARGET nowide)
        add_subdirectory("${UPDATER_ROOT}/core/deps/nowide" updater-nowide EXCLUDE_FROM_ALL)
    endif()
    add_executable(update_http_cancel http_cancel.cpp "${UPDATER_ROOT}/core/oslib/http_client.cpp")
    target_compile_features(update_http_cancel PRIVATE cxx_std_17)
    target_compile_definitions(update_http_cancel PRIVATE NOMINMAX)
    target_include_directories(update_http_cancel PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${UPDATER_ROOT}/core" "${UPDATER_ROOT}/core/deps")
    target_link_libraries(update_http_cancel PRIVATE nowide wininet ws2_32)
    add_test(NAME updater_http_cancel COMMAND update_http_cancel)
    set_tests_properties(updater_http_cancel PROPERTIES TIMEOUT 20)
    add_executable(update_parent dummy_application.cpp)
    add_executable(update_payload dummy_application.cpp)
    target_compile_definitions(update_payload PRIVATE UPDATE_PAYLOAD)
    add_executable(update_failed_payload dummy_application.cpp)
    target_compile_definitions(update_failed_payload PRIVATE UPDATE_PAYLOAD UPDATE_STARTUP_FAILURE)
    find_package(Python3 COMPONENTS Interpreter)
    if(TARGET HollycastUpdater)
        set(UPDATER_HELPER "$<TARGET_FILE:HollycastUpdater>")
    endif()
    if(Python3_FOUND AND UPDATER_HELPER)
        add_test(NAME updater_windows_install COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_install.py"
            --helper "${UPDATER_HELPER}" --parent "$<TARGET_FILE:update_parent>" --payload "$<TARGET_FILE:update_payload>"
            --failed-payload "$<TARGET_FILE:update_failed_payload>")
        set_tests_properties(updater_windows_install PROPERTIES TIMEOUT 240)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME updater_linux_install COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_install.py"
            --helper "${UPDATER_ROOT}/resources/update/install-desktop.sh" --work-root "${CMAKE_CURRENT_BINARY_DIR}"
            --appimage-tool "${UPDATER_ROOT}/appimagetool-x86_64.AppImage")
        set_tests_properties(updater_linux_install PROPERTIES TIMEOUT 90)
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
target_compile_definitions(update_startup_ui PRIVATE IMGUI_ENABLE_TEST_ENGINE)
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
add_test(NAME updater_manual_during_game COMMAND update_startup_ui manual-game)
add_test(NAME updater_shutdown_during_check COMMAND update_startup_ui shutdown)

# Reuse the real native dialog with a fake Java bridge, without pretending the
# host is Android or changing any platform SDK headers.
get_target_property(updater_ui_sources update_startup_ui SOURCES)
get_target_property(updater_ui_includes update_startup_ui INCLUDE_DIRECTORIES)
add_executable(update_play_ui ${updater_ui_sources})
target_compile_features(update_play_ui PRIVATE cxx_std_17)
target_compile_definitions(update_play_ui PRIVATE NOMINMAX IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_ENABLE_TEST_ENGINE HOLLYCAST_TEST_PLAY_UPDATES)
target_include_directories(update_play_ui PRIVATE ${updater_ui_includes})
target_link_libraries(update_play_ui PRIVATE freetype)
if(UNIX)
    target_link_libraries(update_play_ui PRIVATE Threads::Threads)
endif()
add_test(NAME updater_play_without_notes COMMAND update_play_ui play)

set_tests_properties(updater_startup_popup updater_deferred_popup updater_manual_without_startup
    updater_manual_during_game updater_shutdown_during_check updater_play_without_notes PROPERTIES TIMEOUT 15)
