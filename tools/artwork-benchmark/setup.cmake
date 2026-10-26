if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()
if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The artwork benchmark requires macOS")
endif()

enable_language(OBJC OBJCXX)
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE artwork_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(ZLIB_LIBRARY "${artwork_sdk}/usr/lib/libz.tbd" CACHE FILEPATH "macOS SDK zlib for Ninja" FORCE)

function(artwork_add_benchmark)
    if(NOT BUILD_TESTING OR NOT TARGET Hollycast)
        message(FATAL_ERROR "Configure the artwork benchmark with -DENABLE_CTEST=ON")
    endif()
    # The app delegate supplies main(), which would bypass the Google Test entry point.
    get_target_property(artwork_sources Hollycast SOURCES)
    list(FILTER artwork_sources EXCLUDE REGEX "(^|/)SDLApplicationDelegate\\.mm$")
    set_property(TARGET Hollycast PROPERTY SOURCES "${artwork_sources}")

    set(benchmark "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ArtworkBenchmark.cpp")
    target_sources(Hollycast PRIVATE "${benchmark}")
    set_source_files_properties("${benchmark}" PROPERTIES
        COMPILE_OPTIONS "-fno-access-control" SKIP_PRECOMPILE_HEADERS ON)
    file(READ "${CMAKE_SOURCE_DIR}/core/ui/boxart/scraper.h" artwork_scraper_header)
    if(artwork_scraper_header MATCHES "enum[ \t\r\n]+class[ \t\r\n]+OfflineArtState")
        set_source_files_properties("${benchmark}" PROPERTIES COMPILE_DEFINITIONS ARTWORK_BENCH_AFTER=1)
    endif()
    target_link_libraries(Hollycast PRIVATE curl)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL artwork_add_benchmark)
