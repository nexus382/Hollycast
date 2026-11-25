if (ENABLE_SH4_LOOP_PROFILER)
    target_sources(${PROJECT_NAME} PRIVATE sh4_loop_profiler.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SH4_LOOP_PROFILER)
endif()

if (ENABLE_DC_PROFILER)
    target_sources(${PROJECT_NAME} PRIVATE
            dc_profiler.cpp
            dc_profiler.h)

    target_compile_definitions(${PROJECT_NAME} PRIVATE DC_PROFILER)
endif()

if (ENABLE_FC_PROFILER)
    target_sources(${PROJECT_NAME} PRIVATE
            fc_profiler.cpp
            fc_profiler.h)

    target_compile_definitions(${PROJECT_NAME} PRIVATE FC_PROFILER)
endif()
