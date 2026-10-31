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
        src/div32_test.cpp
        src/test_stubs.cpp
        src/serialize_test.cpp
        src/LibretroHelpersTest.cpp
        src/AicaArmTest.cpp
        src/Sh4InterpreterTest.cpp
        src/SorterTest.cpp
        src/MmuTest.cpp
        src/HttpTest.cpp
        src/IniFileTest.cpp
        src/hw/modem/v42Test.cpp
        src/hw/modem/v42bisTest.cpp
        src/network/NetworkServiceTest.cpp
        src/hw/sh4/modules/TimerTest.cpp
        src/imgread/ChdTest.cpp
        src/imgread/CueTest.cpp
        src/imgread/GdiTest.cpp
        src/input/ButtonComboTest.cpp
        src/input/GamepadInputHandlingTest.cpp
        src/input/MultiBindMappingTest.cpp
        src/input/InputDefTest.cpp
        src/input/InputMappingConfigFileTest.cpp
        src/input/InputSetTest.cpp
        src/input/SDLControllerMappingTest.cpp
        src/input/MouseTest.cpp
        src/oslib/I18nTest.cpp
        src/rend/SampleTextureFilterTest.cpp
        src/ui/BoxartTest.cpp
        src/ui/LibraryTest.cpp
        src/ui/ReadinessTest.cpp
        src/ui/SetupChecklistLifecycleTest.cpp
        src/util/PeriodicThreadTest.cpp
        src/util/Md5SumTest.cpp
        src/util/TsQueueTest.cpp
        src/util/WorkerThreadTest.cpp)

if(LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE ../core/ui/dcnet_status.cpp)
endif()

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE src/DcnetProbeTest.cpp)
    target_sources(${PROJECT_NAME} PRIVATE src/ui/SetupChecklistPopupTest.cpp src/ui/CustomThemeTest.cpp src/ui/LibrarySavestateTest.cpp)
    target_sources(${PROJECT_NAME} PRIVATE src/ui/VideoPresetTest.cpp)
    target_sources(${PROJECT_NAME} PRIVATE src/ui/AndroidMenuPopupTest.cpp)
endif()
