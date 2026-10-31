# Native macOS development builds with Ninja.
if(PROJECT_IS_TOP_LEVEL AND APPLE)
    enable_language(OBJC OBJCXX)
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE hollycast_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    set(ZLIB_LIBRARY "${hollycast_sdk}/usr/lib/libz.tbd" CACHE FILEPATH "macOS SDK zlib" FORCE)
endif()
