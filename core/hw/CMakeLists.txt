# Portions Copyright 2026 The Hollycast Authors
target_sources(${PROJECT_NAME} PRIVATE
        hwreg.h)

add_subdirectory(aica)
add_subdirectory(arm7)
add_subdirectory(bba)
add_subdirectory(flashrom)
add_subdirectory(gdrom)
add_subdirectory(holly)
add_subdirectory(maple)
add_subdirectory(mem)
add_subdirectory(modem)
add_subdirectory(naomi)
add_subdirectory(pvr)
add_subdirectory(sh4)
if(USE_HIKARU)
    add_subdirectory(../deps/soundtouch soundtouch)
    add_subdirectory(hikaru)
    add_subdirectory(hikaru/valkyrie)
    target_sources(${PROJECT_NAME} PRIVATE hikaru/frontend.cpp hikaru/sound.cpp hikaru/audio_pacer.cpp
        hikaru/graphics/device.cpp hikaru/graphics/shaders.cpp)
    if(USE_METAL)
        target_sources(${PROJECT_NAME} PRIVATE hikaru/graphics/metal.mm)
        set_source_files_properties(hikaru/graphics/metal.mm TARGET_DIRECTORY ${PROJECT_NAME}
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
    if(USE_VULKAN)
        target_sources(${PROJECT_NAME} PRIVATE hikaru/graphics/vulkan.cpp)
    endif()
    set_source_files_properties(hikaru/audio_pacer.cpp TARGET_DIRECTORY ${PROJECT_NAME}
        PROPERTIES COMPILE_DEFINITIONS SOUNDTOUCH_FLOAT_SAMPLES=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE hollycast_hikaru_io hollycast_hikaru_valkyrie hollycast_soundtouch)
endif()
