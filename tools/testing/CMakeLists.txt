# Portions Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.22)
project(HollycastHarnessTests LANGUAGES CXX)
enable_testing()
add_executable(hollytest-protocol protocol_test.cpp)
target_compile_features(hollytest-protocol PRIVATE cxx_std_17)
target_include_directories(hollytest-protocol PRIVATE ../../core)
add_test(NAME hollytest-protocol COMMAND hollytest-protocol)
