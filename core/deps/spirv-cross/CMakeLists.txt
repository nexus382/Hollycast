# Portions Copyright 2026 The Hollycast Authors
# Pinned at KhronosGroup/SPIRV-Cross 57e4efcc84112abcd426dc5ec248b5339bee04f2.
add_library(hollycast-spirv-cross STATIC
    spirv_cross.cpp
    spirv_parser.cpp
    spirv_cross_parsed_ir.cpp
    spirv_cfg.cpp
    spirv_glsl.cpp
    spirv_hlsl.cpp)
target_include_directories(hollycast-spirv-cross PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(hollycast-spirv-cross PROPERTIES POSITION_INDEPENDENT_CODE ON)
