# Portions Copyright 2026 The Hollycast Authors

if(NOT LIBRETRO)
    # Release builds receive application credentials through the build environment.
    # Keep them out of CMakeCache, compiler command lines, and the source tree.
    set(HOLLYCAST_SS_DEV_ID "$ENV{HOLLYCAST_SCREENSCRAPER_DEV_ID}")
    set(HOLLYCAST_SS_DEV_PASSWORD "$ENV{HOLLYCAST_SCREENSCRAPER_DEV_PASSWORD}")
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
            game_scanner.cpp
            game_scanner.h
            imgui_driver.h
            gui.cpp
            gui.h
            gui_font.cpp
            gui_font.h
            gui_menu.cpp
            gui_menu.h
            gui_achievements.cpp
            vgamepad.cpp
            vgamepad.h
            gui_chat.h
            gui_cheats.cpp
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
            theme_library.h
            theme_library.cpp
            boxart/arcade_scraper.h
            boxart/boxart.cpp
            boxart/boxart.h
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

    if(USE_DISCORD)
        target_sources(${PROJECT_NAME} PRIVATE discord.cpp)
    endif()
endif()
