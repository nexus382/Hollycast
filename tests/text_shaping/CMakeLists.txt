# Portions Copyright 2026 The Hollycast Authors
cmake_minimum_required(VERSION 3.22.1)
project(HollycastTextShapingTests LANGUAGES C CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(HOLLYCAST_ROOT "../.." ABSOLUTE)
set(IMGUI_DIR "${HOLLYCAST_ROOT}/core/deps/imgui")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(OPTION ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${OPTION} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${HOLLYCAST_ROOT}/core/deps/freetype" freetype EXCLUDE_FROM_ALL)
set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
foreach(OPTION UTILS SUBSET RASTER VECTOR GPU)
    set(HB_BUILD_${OPTION} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${HOLLYCAST_ROOT}/core/deps/harfbuzz" harfbuzz EXCLUDE_FROM_ALL)

add_executable(text_shaping_tests text_shaping.cpp
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_cjk.cpp"
    "${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp"
    "${IMGUI_DIR}/misc/freetype/imgui_text_shaping.cpp")
target_include_directories(text_shaping_tests PRIVATE "${IMGUI_DIR}")
target_compile_definitions(text_shaping_tests PRIVATE IMGUI_ENABLE_HARFBUZZ IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(text_shaping_tests PRIVATE harfbuzz freetype)
if(MSVC)
    target_compile_options(text_shaping_tests PRIVATE /utf-8)
endif()
set(TEST_FONTS "${CMAKE_CURRENT_BINARY_DIR}/fonts")
foreach(FONT Roboto-Medium.ttf Roboto-Bold.ttf Roboto-Regular.ttf EncodeSans-wdth-wght.ttf NotoSansDevanagari-Medium.ttf)
    file(ARCHIVE_EXTRACT INPUT "${HOLLYCAST_ROOT}/fonts/${FONT}.zip" DESTINATION "${TEST_FONTS}")
endforeach()
add_test(NAME devanagari_shaping COMMAND text_shaping_tests "${TEST_FONTS}")
