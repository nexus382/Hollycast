# Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.20)
project(ThemeRepositoryHttpTests LANGUAGES CXX)
enable_testing()
find_package(libzip CONFIG REQUIRED)

set(core "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
configure_file("${core}/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)
add_executable(theme_repository_http http_initialization.cpp "${core}/ui/theme_repository.cpp")
target_compile_features(theme_repository_http PRIVATE cxx_std_17)
target_include_directories(theme_repository_http PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${core}" "${core}/deps/json"
    "${core}/deps/stb" "${core}/deps/nowide/include")
target_link_libraries(theme_repository_http PRIVATE libzip::zip)

foreach(scenario cold_start after_shutdown custom_transport)
    add_test(NAME theme_http_${scenario} COMMAND theme_repository_http ${scenario})
endforeach()
