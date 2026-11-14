# Copyright 2026 The Hollycast Authors
# Portions Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.20)
project(PhoneVmuTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(core "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
find_path(ASIO_INCLUDE_DIR asio.hpp HINTS "${core}/deps/asio/asio/include" REQUIRED)

add_executable(phone_vmu_server server.cpp "${core}/network/phone_vmu.cpp" "${core}/oslib/http_server.cpp")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/maple_sync.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_maple_sync_test.py" "${CMAKE_CURRENT_BINARY_DIR}/maple_sync.inc"
    DEPENDS generate_maple_sync_test.py "${core}/hw/maple/maple_devs.cpp" "${core}/hw/maple/maple_devs.h"
    VERBATIM)
target_sources(phone_vmu_server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/maple_sync.inc")
target_compile_features(phone_vmu_server PRIVATE cxx_std_17)
target_include_directories(phone_vmu_server PRIVATE "${core}" "${ASIO_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(phone_vmu_server PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(phone_vmu_server PRIVATE ws2_32 mswsock)
endif()
add_test(NAME phone_vmu_server COMMAND phone_vmu_server)
set_tests_properties(phone_vmu_server PROPERTIES TIMEOUT 30)

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME phone_vmu_browser_logic COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/browser.mjs")
    add_test(NAME phone_vmu_mini_game COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/mini_game.mjs")
    add_test(NAME phone_vmu_wake_lock COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/wake_lock.mjs")
    add_test(NAME phone_vmu_embedded_assets COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embedded_assets.mjs")
    add_test(NAME phone_vmu_progress_sync COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/sync.mjs")
endif()
