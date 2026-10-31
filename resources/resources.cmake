# Portions Copyright 2026 The Hollycast Authors
cmrc_add_resource_library(Hollycast-resources ALIAS Hollycast::res NAMESPACE Hollycast)

cmrc_add_resources(Hollycast-resources
        WHENCE resources
        resources/flash/alienfnt.nvmem.zip	# network settings
        resources/flash/gunsur2.nvmem.zip
        resources/flash/mazan.nvmem.zip		# free play, calibrated
        resources/flash/otrigger.nvmem.zip
        resources/flash/wldkicks.nvmem.zip	# free play
        resources/flash/wldkicksj.nvmem.zip	# free play
        resources/flash/wldkicksu.nvmem.zip	# free play
        resources/flash/f355.nvmem.zip		# printer on
        resources/flash/f355twin.nvmem.zip	# printer on
        resources/flash/f355twn2.nvmem.zip	# printer on
        resources/flash/dirtypig.nvmem.zip	# 4 players
        resources/flash/dirtypig.nvmem2.zip
        resources/flash/vf4.nvmem.zip		# card all day, stage select
        resources/flash/vf4evob.nvmem.zip	# card all day, stage select
        resources/flash/vf4tuned.nvmem.zip  # card all day, stage select, 45 sec time limit, 3 match
        resources/flash/magicpop.nvmem.zip	# debug: comm and all errors disabled
        resources/flash/ochaken.nvmem.zip	# debug: comm and all errors disabled
        resources/flash/puyomedal.nvmem.zip	# debug: comm and all errors disabled
        resources/flash/unomedal.nvmem.zip	# debug: comm and all errors disabled
        resources/flash/westdrmg.nvmem.zip	# debug: comm and all errors disabled
        resources/flash/smarinef.nvmem.zip	# standard cabinet
        resources/picture/f355_print_template.png
        resources/picture/hollycast_ascii.txt)

cmrc_add_resources(Hollycast-resources
        fonts/printer_ascii8x16.bin.zip
        fonts/printer_ascii12x24.bin.zip
        fonts/printer_kanji16x16.bin.zip
        fonts/printer_kanji24x24.bin.zip)

if(NOT LIBRETRO)
    cmrc_add_resources(Hollycast-resources
            fonts/NotoSansCJK-Regular.ttc.zip
            fonts/NotoSansCJK-LICENSE.txt
            fonts/Roboto-Medium.ttf.zip
            fonts/Roboto-Bold.ttf.zip
            fonts/Roboto-Regular.ttf.zip
            fonts/Jura-wght.ttf.zip
            fonts/EncodeSans-wdth-wght.ttf.zip
            fonts/fa-solid-900.ttf.zip)
    cmrc_add_resources(Hollycast-resources
            WHENCE resources
            resources/picture/hollycast-launch.png)
    if(ANDROID OR IOS)
        cmrc_add_resources(Hollycast-resources
                WHENCE resources
                resources/picture/buttons.png
                resources/picture/buttons-arcade.png
                resources/picture/blkclrDC.png
                resources/picture/drkgreywhtsolidDC.png
                resources/picture/greywhitesolidDC.png
                resources/picture/briteneonDC.png
                resources/picture/NeondimDC.png
                resources/picture/blkgryARC.png
                resources/picture/blkcolorARC.png
                resources/picture/grywhitesldARC.png
                resources/picture/whitelineARC.png
                resources/picture/neonARC.png)
    endif()
endif()

cmrc_add_resources(Hollycast-resources fonts/biosfont.bin.zip)

cmrc_add_resources(Hollycast-resources
	WHENCE resources
	resources/i18n/fr.po
	resources/i18n/hu.po
	resources/i18n/ja.po
	resources/i18n/pt_BR.po
	resources/i18n/sv.po
	resources/i18n/zh_CN.po
	resources/i18n/zh_HK.po
	resources/i18n/zh_TW.po)

cmrc_add_resources(Hollycast-resources
        WHENCE resources
        resources/themes/Dark.json
        resources/themes/Light.json
        resources/themes/Dreamcast.json
        "resources/themes/High Contrast.json"
        resources/themes/Nintendo.json
        "resources/themes/Aqua Chill.json")

# Ship the complete library-theme packages with every standalone release. Hash
# content so startup can update bundled assets once without per-frame disk scans.
if(NOT LIBRETRO)
    file(GLOB_RECURSE HOLLYCAST_LIBRARY_THEME_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/themes/*")
    list(SORT HOLLYCAST_LIBRARY_THEME_FILES)
    set(HOLLYCAST_LIBRARY_THEME_HASHES "")
    foreach(theme_file IN LISTS HOLLYCAST_LIBRARY_THEME_FILES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${theme_file}")
        file(SHA256 "${theme_file}" theme_hash)
        file(RELATIVE_PATH theme_relative "${CMAKE_CURRENT_SOURCE_DIR}/themes" "${theme_file}")
        string(APPEND HOLLYCAST_LIBRARY_THEME_HASHES "${theme_relative}:${theme_hash}\n")
    endforeach()
    string(SHA256 HOLLYCAST_LIBRARY_THEME_VERSION "${HOLLYCAST_LIBRARY_THEME_HASHES}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bundled-library-themes")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bundled-library-themes/.version" "${HOLLYCAST_LIBRARY_THEME_VERSION}")
    cmrc_add_resources(Hollycast-resources
        WHENCE "${CMAKE_CURRENT_SOURCE_DIR}/themes" PREFIX library-themes
        ${HOLLYCAST_LIBRARY_THEME_FILES})
    cmrc_add_resources(Hollycast-resources
        WHENCE "${CMAKE_CURRENT_BINARY_DIR}/bundled-library-themes" PREFIX library-themes
        "${CMAKE_CURRENT_BINARY_DIR}/bundled-library-themes/.version")
endif()
