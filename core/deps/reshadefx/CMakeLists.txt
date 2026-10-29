# Portions Copyright 2026 The Hollycast Authors

add_library(hollycast-reshadefx STATIC
    effect_codegen_glsl.cpp
    effect_expression.cpp
    effect_lexer.cpp
    effect_parser_exp.cpp
    effect_parser_stmt.cpp
    effect_preprocessor.cpp
    effect_symbol_table.cpp)
target_compile_features(hollycast-reshadefx PUBLIC cxx_std_17)
target_include_directories(hollycast-reshadefx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(hollycast-reshadefx PROPERTIES POSITION_INDEPENDENT_CODE ON)
