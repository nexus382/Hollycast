# Portions Copyright 2026 The Hollycast Authors

if(NOT LIBRETRO)
    if(ENABLE_CTEST)
        target_compile_definitions(${PROJECT_NAME} PRIVATE HOLLYCAST_DISABLE_STARTUP_UPDATES)
    endif()
    option(HOLLYCAST_REQUIRE_SCREENSCRAPER "Require application access in official standalone builds"
            "$ENV{HOLLYCAST_REQUIRE_SCREENSCRAPER}")
    # Release builds receive application credentials through the build environment.
    # Keep them out of CMakeCache, compiler command lines, and the source tree.
    set(HOLLYCAST_SS_DEV_ID "$ENV{HOLLYCAST_SCREENSCRAPER_DEV_ID}")
    set(HOLLYCAST_SS_DEV_PASSWORD "$ENV{HOLLYCAST_SCREENSCRAPER_DEV_PASSWORD}")
    if(HOLLYCAST_REQUIRE_SCREENSCRAPER
            AND (HOLLYCAST_SS_DEV_ID STREQUAL "" OR HOLLYCAST_SS_DEV_PASSWORD STREQUAL ""))
        message(FATAL_ERROR "Official Hollycast builds require ScreenScraper application credentials for guest access.")
    endif()
    if((HOLLYCAST_SS_DEV_ID STREQUAL "" AND NOT HOLLYCAST_SS_DEV_PASSWORD STREQUAL "")
            OR (NOT HOLLYCAST_SS_DEV_ID STREQUAL "" AND HOLLYCAST_SS_DEV_PASSWORD STREQUAL ""))
        message(FATAL_ERROR "ScreenScraper build provisioning requires both developer environment variables.")
    endif()
    foreach(field HOLLYCAST_SS_DEV_ID HOLLYCAST_SS_DEV_PASSWORD)
        string(REPLACE "\\" "\\\\" ${field} "${${field}}")
        string(REPLACE "\"" "\\\"" ${field} "${${field}}")
        string(REPLACE "\n" "\\n" ${field} "${${field}}")
        string(REPLACE "\r" "\\r" ${field} "${${field}}")
        string(REPLACE "\t" "\\t" ${field} "${${field}}")
    endforeach()
    configure_file(boxart/screenscraper_build_credentials.h.in
            generated/screenscraper_build_credentials.h @ONLY)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    unset(HOLLYCAST_SS_DEV_ID)
    unset(HOLLYCAST_SS_DEV_PASSWORD)

    target_sources(${PROJECT_NAME} PRIVATE
            app_updater.cpp
            app_updater.h
            app_update_catalog.cpp
            app_update_install.cpp
            app_update_cleanup.cpp
            app_update_package.h
            game_scanner.cpp
            game_scanner.h
            imgui_driver.h
            gui.cpp
            library.cpp
            gui.h
            gui_font.cpp
            gui_font.h
            gui_menu.cpp
            gui_menu.h
            gui_achievements.cpp
            dcnet_status.cpp
            dcnet_status.h
            dcnet_probe.cpp
            dcnet_probe.h
            gui_dcnet.cpp
            gui_dcnet.h
            remoteplay/remote_play.cpp
            remoteplay/remote_play.h
            remoteplay/remote_play_page.h
            remoteplay/remote_play_protocol.h
            remoteplay/cloudflare_tunnel.cpp
            remoteplay/cloudflare_tunnel.h
            remoteplay/webrtc_peer.cpp
            remoteplay/webrtc_peer.h
            remoteplay/audio_stream.cpp
            remoteplay/audio_stream.h
            remoteplay/h264_encoder.h
            vgamepad.cpp
            vgamepad.h
            gui_chat.h
            gui_cheats.cpp
            gui_debugger.cpp
            gui_debugger.h
            gui_util.cpp
            gui_util.h
            mainui.cpp
            mainui.h
            performance_overlay.cpp
            performance_overlay.h
            readiness.h
            setup_checklist_lifecycle.h
            setup_checklist_popup.h
            settings.cpp
            settings.h
            settings_about.cpp
            settings_audio.cpp
            settings_controls.cpp
            settings_general.cpp
            custom_theme.cpp
            custom_theme.h
            settings_network.cpp
            settings_video.cpp
            settings_new.cpp
            settings_new.h
            video_preset.cpp
            video_preset.h
            theme.h
            theme.cpp
            theme_parser.cpp
            theme_repository.h
            theme_repository.cpp
            theme_browser.h
            theme_browser.cpp
            theme_library.h
            theme_library.cpp
            theme_artwork.h
            theme_artwork.cpp
            boxart/arcade_scraper.h
            boxart/boxart.cpp
            boxart/boxart.h
            boxart/artwork_adapter.cpp
            boxart/artwork_adapter.h
            boxart/gamesdb.cpp
            boxart/gamesdb.h
            boxart/screenscraper.cpp
            boxart/screenscraper.h
            boxart/launchbox.cpp
            boxart/launchbox.h
            boxart/pvrparser.h
            boxart/scraper.cpp
            boxart/scraper.h
            boxart/vmu_icon.cpp
            boxart/vmu_icon.h)

    # WebRTC Remote Play: libdatachannel + a platform H.264 encoder.
    # Currently wired on macOS (VideoToolbox); other platforms keep the
    # MJPEG stream until an encoder lands there.
    option(HOLLYCAST_REMOTEPLAY_WEBRTC "Enable WebRTC for Remote Play" ON)
    if(APPLE AND NOT IOS AND HOLLYCAST_REMOTEPLAY_WEBRTC
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../core/deps/libdatachannel/CMakeLists.txt")
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../core/deps/opus/CMakeLists.txt")
            message(FATAL_ERROR "Remote Play audio requires Opus. Run git submodule update --init core/deps/opus")
        endif()
        function(add_remoteplay_opus)
            set(BUILD_TESTING OFF)
            set(BUILD_SHARED_LIBS OFF)
            set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
            set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../core/deps/opus"
                    "${CMAKE_CURRENT_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)
        endfunction()
        add_remoteplay_opus()
        target_sources(${PROJECT_NAME} PRIVATE remoteplay/h264_encoder_vt.mm remoteplay/opus_encoder.cpp)
        # Hollycast already vendors libjuice: reuse it instead of letting
        # libdatachannel create a second "juice" target.
        if(TARGET juice-static AND NOT TARGET LibJuice::LibJuice)
            add_library(LibJuice::LibJuice ALIAS juice-static)
        endif()
        set(USE_SYSTEM_JUICE ON CACHE BOOL "" FORCE)
        set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
        set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
        set(NO_TESTS ON CACHE BOOL "" FORCE)
        if(NOT OPENSSL_ROOT_DIR AND EXISTS "/opt/homebrew/opt/openssl@3")
            set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
        elseif(NOT OPENSSL_ROOT_DIR AND EXISTS "/usr/local/opt/openssl@3")
            set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
        endif()
        # The top-level HTTP configuration also has ENABLE_OPENSSL and
        # defaults it to OFF on Apple. libdatachannel's bundled libSRTP
        # needs its crypto backend enabled to support the GCM profiles it
        # advertises during DTLS negotiation. Keep this override local to
        # this directory, without changing Hollycast's HTTP backend.
        if(NOT USE_SYSTEM_SRTP)
            if(USE_MBEDTLS)
                set(ENABLE_MBEDTLS ON)
            elseif(NOT USE_GNUTLS)
                set(ENABLE_OPENSSL ON)
            endif()
        endif()
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../core/deps/libdatachannel"
                "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel" EXCLUDE_FROM_ALL)
        target_link_libraries(${PROJECT_NAME} PRIVATE datachannel-static opus
                "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
        target_compile_definitions(${PROJECT_NAME} PRIVATE HOLLYCAST_HAVE_WEBRTC=1)
    else()
        target_sources(${PROJECT_NAME} PRIVATE remoteplay/h264_encoder_stub.cpp)
    endif()

    if(USE_DISCORD)
        target_sources(${PROJECT_NAME} PRIVATE discord.cpp)
    endif()
endif()
