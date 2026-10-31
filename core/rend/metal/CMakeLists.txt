set(METAL_SOURCES
    metal_context.mm
    metal_imgui.mm
    metal_renderer.mm
    metal_shaders.mm
    metal_texture.mm
    metal_modvol.mm
    metal_oit.mm
    metal_oit_shaders.mm
    metal_overlay.mm)
target_sources(${PROJECT_NAME} PRIVATE ${METAL_SOURCES})
set_source_files_properties(${METAL_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_METAL)
find_library(HOLLYCAST_METAL_FRAMEWORK Metal REQUIRED)
find_library(HOLLYCAST_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} ${HOLLYCAST_QUARTZCORE_FRAMEWORK})

# Explicit target: offscreen GPU checks are not part of release builds.
foreach(check texture_upload regular_shader modifier_shader)
    add_executable(metal_${check}_smoke EXCLUDE_FROM_ALL
        "${PROJECT_SOURCE_DIR}/tests/metal/${check}_smoke.mm")
    target_include_directories(metal_${check}_smoke PRIVATE "${PROJECT_SOURCE_DIR}/core")
    target_compile_options(metal_${check}_smoke PRIVATE -fobjc-arc)
    target_link_libraries(metal_${check}_smoke PRIVATE
        ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")
endforeach()
target_sources(metal_regular_shader_smoke PRIVATE metal_shaders.mm)
set_source_files_properties(metal_shaders.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
foreach(check cull_compare flat_punch_regression alpha_semantics_regression native_depth_regression)
    add_executable(metal_${check} EXCLUDE_FROM_ALL
        "${PROJECT_SOURCE_DIR}/tests/metal/${check}.mm" metal_shaders.mm)
    target_include_directories(metal_${check} PRIVATE
        "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
        "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
    target_compile_options(metal_${check} PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -Wno-deprecated-declarations
        -ffunction-sections -fdata-sections)
    target_link_options(metal_${check} PRIVATE -Wl,-dead_strip)
    target_link_libraries(metal_${check} PRIVATE
        ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")
endforeach()
target_sources(metal_flat_punch_regression PRIVATE "${PROJECT_SOURCE_DIR}/core/rend/sorter.cpp")
target_link_libraries(metal_cull_compare PRIVATE "-framework OpenGL")

# Modifier-volume culling comparison against the OpenGL reference.
add_executable(metal_modvol_cull_compare EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/modvol_cull_compare.mm")
target_include_directories(metal_modvol_cull_compare PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
target_compile_options(metal_modvol_cull_compare PRIVATE -fobjc-arc -Wno-deprecated-declarations)
target_link_libraries(metal_modvol_cull_compare PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation" "-framework OpenGL")

add_executable(metal_target_preservation_regression EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/target_preservation_regression.mm")
target_include_directories(metal_target_preservation_regression PRIVATE "${PROJECT_SOURCE_DIR}/core")
target_compile_options(metal_target_preservation_regression PRIVATE -fobjc-arc)
target_link_libraries(metal_target_preservation_regression PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")

add_executable(metal_reconstruction_compare EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/reconstruction_compare.mm")
target_include_directories(metal_reconstruction_compare PRIVATE "${PROJECT_SOURCE_DIR}/core")
target_compile_options(metal_reconstruction_compare PRIVATE -fobjc-arc -Wno-deprecated-declarations)
target_link_libraries(metal_reconstruction_compare PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation" "-framework OpenGL")

add_executable(metal_renderer_type_test EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/renderer_type_test.cpp")
target_include_directories(metal_renderer_type_test PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps/nowide/include")

add_executable(metal_oit_buffer_regression EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/oit_buffer_regression.mm" metal_oit.mm metal_oit_shaders.mm)
target_include_directories(metal_oit_buffer_regression PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
target_compile_options(metal_oit_buffer_regression PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -ffunction-sections -fdata-sections)
target_link_options(metal_oit_buffer_regression PRIVATE -Wl,-dead_strip)
target_link_libraries(metal_oit_buffer_regression PRIVATE ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")

add_executable(metal_oit_modifier_regression EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/oit_modifier_regression.mm" metal_oit_shaders.mm)
target_include_directories(metal_oit_modifier_regression PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
target_compile_options(metal_oit_modifier_regression PRIVATE -fobjc-arc)
target_link_libraries(metal_oit_modifier_regression PRIVATE ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")

add_executable(metal_oit_blend_matrix EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/oit_blend_matrix.mm" metal_oit.mm metal_oit_shaders.mm)
target_include_directories(metal_oit_blend_matrix PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
target_compile_options(metal_oit_blend_matrix PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -ffunction-sections -fdata-sections)
target_link_options(metal_oit_blend_matrix PRIVATE -Wl,-dead_strip)
target_link_libraries(metal_oit_blend_matrix PRIVATE ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")

add_executable(metal_oit_collect_regression EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/oit_collect_regression.mm"
    metal_shaders.mm metal_oit.mm metal_oit_shaders.mm)
target_include_directories(metal_oit_collect_regression PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
target_compile_options(metal_oit_collect_regression PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -ffunction-sections -fdata-sections)
target_link_options(metal_oit_collect_regression PRIVATE -Wl,-dead_strip)
target_link_libraries(metal_oit_collect_regression PRIVATE ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")

add_executable(metal_overlay_smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/overlay_smoke.mm" metal_overlay.mm)
target_include_directories(metal_overlay_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui" "${PROJECT_SOURCE_DIR}/core/deps/SDL/include")
target_compile_definitions(metal_overlay_smoke PRIVATE USE_SDL)
target_compile_options(metal_overlay_smoke PRIVATE -fobjc-arc)
target_link_libraries(metal_overlay_smoke PRIVATE ${HOLLYCAST_METAL_FRAMEWORK}
    ${HOLLYCAST_QUARTZCORE_FRAMEWORK} "-framework Foundation")
if(TARGET SDL2::SDL2-static)
    target_link_libraries(metal_overlay_smoke PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(metal_overlay_smoke PRIVATE SDL2::SDL2)
else()
    target_include_directories(metal_overlay_smoke PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

add_executable(metal_oit_depth_replay_regression EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/oit_depth_replay_regression.mm" metal_oit.mm metal_oit_shaders.mm)
target_include_directories(metal_oit_depth_replay_regression PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include" "${PROJECT_SOURCE_DIR}/core/deps/glm")
target_compile_options(metal_oit_depth_replay_regression PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -ffunction-sections -fdata-sections)
target_link_options(metal_oit_depth_replay_regression PRIVATE -Wl,-dead_strip)
target_link_libraries(metal_oit_depth_replay_regression PRIVATE ${HOLLYCAST_METAL_FRAMEWORK} "-framework Foundation")

# Link-time Metal device override checks unavailable-device behavior.
add_executable(metal_context_unavailable_smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/context_unavailable_smoke.mm" metal_context.mm)
target_include_directories(metal_context_unavailable_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/glm" "${PROJECT_SOURCE_DIR}/core/deps/imgui"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include")
target_compile_definitions(metal_context_unavailable_smoke PRIVATE USE_SDL)
target_compile_options(metal_context_unavailable_smoke PRIVATE -fobjc-arc)
target_link_libraries(metal_context_unavailable_smoke PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} ${HOLLYCAST_QUARTZCORE_FRAMEWORK}
    "-framework Cocoa" "-framework Foundation")
if(TARGET SDL2::SDL2-static)
    target_link_libraries(metal_context_unavailable_smoke PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(metal_context_unavailable_smoke PRIVATE SDL2::SDL2)
else()
    target_include_directories(metal_context_unavailable_smoke PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(metal_context_unavailable_smoke PRIVATE ${SDL2_LIBRARIES})
endif()

add_executable(metal_framebuffer_dither_regression EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/framebuffer_dither_regression.cpp")
target_include_directories(metal_framebuffer_dither_regression PRIVATE "${PROJECT_SOURCE_DIR}/core")
target_compile_definitions(metal_framebuffer_dither_regression PRIVATE NDEBUG)

add_custom_target(metal-smoke
    COMMAND $<TARGET_FILE:metal_renderer_type_test>
    COMMAND $<TARGET_FILE:metal_framebuffer_dither_regression>
    COMMAND $<TARGET_FILE:metal_context_unavailable_smoke>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_overlay_smoke>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_oit_depth_replay_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_oit_collect_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_oit_blend_matrix>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_oit_buffer_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_oit_modifier_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_reconstruction_compare> "${PROJECT_SOURCE_DIR}/core/rend/gles/gles.cpp"
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_native_depth_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_texture_upload_smoke>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_regular_shader_smoke>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_modifier_shader_smoke>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_cull_compare>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_modvol_cull_compare>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_target_preservation_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_flat_punch_regression>
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_alpha_semantics_regression>
    DEPENDS metal_framebuffer_dither_regression metal_context_unavailable_smoke metal_overlay_smoke metal_renderer_type_test metal_reconstruction_compare metal_native_depth_regression metal_oit_buffer_regression metal_oit_modifier_regression metal_oit_blend_matrix metal_oit_collect_regression metal_oit_depth_replay_regression
        metal_texture_upload_smoke metal_regular_shader_smoke metal_modifier_shader_smoke
        metal_cull_compare metal_modvol_cull_compare metal_target_preservation_regression
        metal_flat_punch_regression metal_alpha_semantics_regression
    USES_TERMINAL)


# Opens a real SDL window. Run explicitly; it is not part of offscreen metal-smoke.
add_executable(metal_context_smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/context_smoke.mm" metal_context.mm)
target_include_directories(metal_context_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/glm" "${PROJECT_SOURCE_DIR}/core/deps/imgui"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include")
target_compile_definitions(metal_context_smoke PRIVATE USE_SDL)
target_compile_options(metal_context_smoke PRIVATE -fobjc-arc)
target_link_libraries(metal_context_smoke PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} ${HOLLYCAST_QUARTZCORE_FRAMEWORK}
    "-framework Cocoa" "-framework Foundation")
if(TARGET SDL2::SDL2-static)
    target_link_libraries(metal_context_smoke PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(metal_context_smoke PRIVATE SDL2::SDL2)
else()
    target_include_directories(metal_context_smoke PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(metal_context_smoke PRIVATE ${SDL2_LIBRARIES})
endif()
add_custom_target(metal-context-smoke
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_context_smoke>
    DEPENDS metal_context_smoke
    USES_TERMINAL)

# Real presentation geometry and routing, isolated from the application state.
add_executable(metal_context_presentation_smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/context_presentation_smoke.mm" metal_context.mm
    "${PROJECT_SOURCE_DIR}/core/rend/transform_matrix.cpp")
target_include_directories(metal_context_presentation_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/glm" "${PROJECT_SOURCE_DIR}/core/deps/imgui"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include")
target_compile_definitions(metal_context_presentation_smoke PRIVATE USE_SDL VIDEO_ROUTING TARGET_MAC)
target_compile_options(metal_context_presentation_smoke PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -ffunction-sections -fdata-sections)
target_link_options(metal_context_presentation_smoke PRIVATE -Wl,-dead_strip)
target_link_libraries(metal_context_presentation_smoke PRIVATE
    ${HOLLYCAST_METAL_FRAMEWORK} ${HOLLYCAST_QUARTZCORE_FRAMEWORK}
    "-framework Cocoa" "-framework Foundation")
if(TARGET SDL2::SDL2-static)
    target_link_libraries(metal_context_presentation_smoke PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(metal_context_presentation_smoke PRIVATE SDL2::SDL2)
else()
    target_include_directories(metal_context_presentation_smoke PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(metal_context_presentation_smoke PRIVATE ${SDL2_LIBRARIES})
endif()
add_custom_target(metal-context-presentation-smoke
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_context_presentation_smoke>
    DEPENDS metal_context_presentation_smoke USES_TERMINAL)

# Real ImGui frame snapshots, atlas updates and Metal presentation.
add_executable(metal_imgui_smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tests/metal/imgui_smoke.mm" metal_context.mm metal_imgui.mm
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/imgui.cpp"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/imgui_draw.cpp"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/imgui_cjk.cpp"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/imgui_tables.cpp"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/imgui_widgets.cpp"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/misc/freetype/imgui_freetype.cpp"
    "${PROJECT_SOURCE_DIR}/core/deps/imgui/misc/freetype/imgui_text_shaping.cpp")
target_include_directories(metal_imgui_smoke PRIVATE
    "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/deps"
    "${PROJECT_SOURCE_DIR}/core/deps/glm" "${PROJECT_SOURCE_DIR}/core/deps/imgui"
    "${PROJECT_SOURCE_DIR}/core/deps/nowide/include")
target_compile_definitions(metal_imgui_smoke PRIVATE USE_SDL VIDEO_ROUTING TARGET_MAC IMGUI_ENABLE_HARFBUZZ)
target_compile_options(metal_imgui_smoke PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(metal_imgui_smoke PRIVATE freetype harfbuzz
    ${HOLLYCAST_METAL_FRAMEWORK} ${HOLLYCAST_QUARTZCORE_FRAMEWORK}
    "-framework Cocoa" "-framework Foundation")
if(TARGET SDL2::SDL2-static)
    target_link_libraries(metal_imgui_smoke PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(metal_imgui_smoke PRIVATE SDL2::SDL2)
else()
    target_include_directories(metal_imgui_smoke PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(metal_imgui_smoke PRIVATE ${SDL2_LIBRARIES})
endif()
add_custom_target(metal-imgui-smoke
    COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 $<TARGET_FILE:metal_imgui_smoke>
    DEPENDS metal_imgui_smoke USES_TERMINAL)
