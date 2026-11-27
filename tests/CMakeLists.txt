# Portions Copyright 2026 The Hollycast Authors
option(BUILD_GMOCK "Builds the googlemock subproject" OFF)
option(INSTALL_GTEST "Enable installation of googletest. (Projects embedding googletest may want to turn this OFF.)" OFF)
if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE GTest::gtest_main)

# HttpTest calls libcurl directly; Apple builds use the native HTTP client in the core.
if(APPLE)
    find_package(CURL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
endif()
        
target_include_directories(${PROJECT_NAME} PUBLIC inc)
target_sources(${PROJECT_NAME} PRIVATE
        src/DcnetStatusTest.cpp
        src/CheatManagerTest.cpp
        src/ConfigFileTest.cpp
        src/OptionTest.cpp
        src/div32_test.cpp
        src/test_stubs.cpp
        src/serialize_test.cpp
        src/LibretroHelpersTest.cpp
        src/MapleGgpoTest.cpp
        src/audio/AlsaCaptureTest.cpp
        src/GgpoAdaptiveDelayTest.cpp
        src/GgpoResumeBarrierTest.cpp
        src/SessionStateTest.cpp
        src/AicaArmTest.cpp
        src/Sh4InterpreterTest.cpp
        src/AssemblyDebuggerTest.cpp
        src/SorterTest.cpp
        src/MmuTest.cpp
        src/HttpTest.cpp
        src/IniFileTest.cpp
        src/rend/PostProcessingChainTest.cpp
        src/rend/TexCacheEvictionTest.cpp
        src/rend/MetalShaderCompilerTest.cpp
        src/rend/ShaderLayerTest.cpp
        src/rend/ReShadeCompilerTest.cpp
        src/rend/OpenGLShaderChainTest.cpp
        src/rend/DirectXShaderChainTest.cpp
		src/rend/SlangFrontendTest.cpp
        src/hw/modem/v42Test.cpp
        src/hw/modem/v42bisTest.cpp
        src/network/NetworkServiceTest.cpp
        src/network/DnsParserTest.cpp
        src/hw/sh4/modules/TimerTest.cpp
        src/hw/NaomiRomReadTest.cpp
        src/imgread/ChdTest.cpp
        src/imgread/CueTest.cpp
        src/imgread/GdiTest.cpp
        src/imgread/IsoFsTest.cpp
        src/input/ButtonComboTest.cpp
        src/input/GamepadInputHandlingTest.cpp
        src/input/HikaruGamepadPresetTest.cpp
        src/input/MultiBindMappingTest.cpp
        src/input/InputDefTest.cpp
        src/input/InputMappingConfigFileTest.cpp
        src/input/InputSetTest.cpp
        src/input/SDLControllerMappingTest.cpp
        src/input/MouseTest.cpp
        src/oslib/I18nTest.cpp
        src/oslib/DirectoryTest.cpp
        src/oslib/SavestateWriteTest.cpp
        src/rend/SampleTextureFilterTest.cpp
        src/rend/TextureAtlasTest.cpp
        src/rend/CustomTextureDumpTest.cpp
        src/ui/BoxartTest.cpp
        src/ui/ScreenScraperTest.cpp
        src/ui/LaunchBoxTest.cpp
        src/ui/ArtworkAdapterTest.cpp
        src/ui/TheGamesDbTest.cpp
        src/ui/LibraryTest.cpp
        src/ui/LibrarySortTest.cpp
        src/ui/ReadinessTest.cpp
        src/ui/SetupChecklistLifecycleTest.cpp
        src/util/PeriodicThreadTest.cpp
        src/util/Md5SumTest.cpp
        src/util/TsQueueTest.cpp
        src/util/WorkerThreadTest.cpp)

if(LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE ../core/ui/dcnet_status.cpp)
elseif(NOT ANDROID AND NOT IOS AND NOT NINTENDO_SWITCH AND NOT WINDOWS_STORE)
    # The desktop test executable can install a frontend VFS for this suite.
    # With no frontend installed, AllStorage continues to use native files.
    set(storage_test_source ${CMAKE_CURRENT_SOURCE_DIR}/src/LibretroStorageTest.cpp)
    set(vfs_backend_source ${PROJECT_SOURCE_DIR}/shell/libretro/storage.cpp)
    target_sources(${PROJECT_NAME} PRIVATE ${storage_test_source} ${vfs_backend_source} ${libretro_vfs_sources})
    set_property(SOURCE ${storage_test_source} ${vfs_backend_source} ${libretro_vfs_sources}
        DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/core/deps/libretro-common/include)
    set_property(SOURCE ${PROJECT_SOURCE_DIR}/core/oslib/storage.cpp ${vfs_backend_source} ${libretro_vfs_sources}
        DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY COMPILE_DEFINITIONS LIBRETRO)
    if(ENABLE_CTEST)
        # Build with: cmake --build <build-dir> --target Hollycast
        # Run with: ctest --test-dir <build-dir> -R '^libretro_storage$' --output-on-failure
        add_test(NAME libretro_storage
            COMMAND $<TARGET_FILE:${PROJECT_NAME}> --gtest_filter=LibretroStorageTest.*)
    endif()
endif()

if(USE_HIKARU)
    target_sources(${PROJECT_NAME} PRIVATE src/HikaruStateTest.cpp src/HikaruCardStateTest.cpp src/HikaruAudioPacerTest.cpp src/HikaruSoundFrameTest.cpp src/HikaruAuditTest.cpp src/HikaruInputBindingsTest.cpp src/HikaruCabinetSettingsTest.cpp src/HikaruFrameWorkerTest.cpp)
endif()

if(USE_HIKARU AND USE_VULKAN AND NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE src/HikaruVulkanTest.cpp)
    # Expose the generic-shader oracle only in the native test executable.
    set_property(SOURCE "${PROJECT_SOURCE_DIR}/core/hw/hikaru/graphics/vulkan.cpp"
        DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS HIKARU_NATIVE_TESTING)
endif()

if(USE_HIKARU AND USE_DX11 AND WIN32 AND NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE src/HikaruDx11Test.cpp)
    set_property(SOURCE "${PROJECT_SOURCE_DIR}/core/hw/hikaru/graphics/dx11.cpp"
        DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS HIKARU_NATIVE_TESTING)
endif()

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
        src/RewindTest.cpp
        src/util/RewindBufferTest.cpp
        src/hw/VmuMigrationTest.cpp
        src/oslib/VmuPathTest.cpp
        src/DcnetProbeTest.cpp
        src/network/HttpTimeoutTest.cpp
        src/ui/SetupChecklistPopupTest.cpp
        src/ui/SettingsPopupTest.cpp
        src/ui/CustomThemeTest.cpp
        src/ui/LibrarySavestateTest.cpp
        src/ui/LibraryImageTest.cpp
        src/ui/PvrParserTest.cpp
        src/ui/GameScannerTest.cpp
        src/ui/ThemeTest.cpp
        src/ui/ThemeLibraryTest.cpp
        src/ui/ThemeArtworkTest.cpp
        src/ui/ThemeInstallTest.cpp
        src/ui/ThemeRepositoryTest.cpp
        src/ui/VideoPresetTest.cpp
        src/ui/AndroidMenuPopupTest.cpp
        src/ui/ShaderPreviewLayoutTest.cpp)
endif()

if(USE_METAL)
    target_sources(${PROJECT_NAME} PRIVATE src/rend/MetalShaderChainTest.mm)
    set_source_files_properties(src/rend/MetalShaderChainTest.mm TARGET_DIRECTORY ${PROJECT_NAME}
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# Hikaru bring-up checks also run without the rest of the emulator's dependencies.
add_subdirectory(hikaru)
