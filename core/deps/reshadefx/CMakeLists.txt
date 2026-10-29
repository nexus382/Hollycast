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

# Some supported standard libraries omit floating-point charconv or require a
# newer OS deployment target. Detect the actual toolchain instead of its version.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
    #include <charconv>
    int main() {
        const char text[] = "0.5";
        float value = 0;
        std::from_chars(text, text + 3, value);
        char output[64];
        std::to_chars(output, output + sizeof(output), value, std::chars_format::scientific, 8);
        return value == 0.5f ? 0 : 1;
    }
]=] HOLLYCAST_HAVE_FLOAT_CHARCONV)
if(HOLLYCAST_HAVE_FLOAT_CHARCONV)
    target_compile_definitions(hollycast-reshadefx PRIVATE HOLLYCAST_HAVE_FLOAT_CHARCONV=1)
endif()
