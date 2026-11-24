cmake_minimum_required(VERSION 3.24)
# The version: VERSION holds the current one ("_b" is added to beta builds by tools/make_dist.ps1)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(RealEngine VERSION ${RE_VERSION} LANGUAGES C CXX)
set(RE_VERSION_STRING "${RE_VERSION}" CACHE STRING "The version shown and checked by the updater (a beta adds _b)")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

find_package(Vulkan 1.3 REQUIRED)
if(NOT MSVC) # the MSVC names the code uses (_stricmp, __declspec(thread)...) on clang (the Android NDK): src/re_compat.h
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-include$<SEMICOLON>${CMAKE_CURRENT_SOURCE_DIR}/src/re_compat.h>)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-unused-parameter> $<$<COMPILE_LANGUAGE:C>:-Wno-missing-field-initializers>)
endif()

# --- GLFW (window and input), copied from the Tlender --------------------------
# Android (notas-dev/android.md): the same API over the NDK's native_app_glue (android/glfw_android.c, the Tlender
# Mobile's): the touch screen reaches the game as GLFW's mouse, keys and the touch points
if(ANDROID)
    add_library(glfw STATIC ${CMAKE_CURRENT_SOURCE_DIR}/android/glfw_android.c ${CMAKE_CURRENT_SOURCE_DIR}/android/vk_android.c
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/android/vk_android.c PROPERTIES COMPILE_OPTIONS "-DRE_NO_VK_RENAME")
    target_include_directories(glfw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/include ${CMAKE_CURRENT_SOURCE_DIR}/android
        ${ANDROID_NDK}/sources/android/native_app_glue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(glfw PUBLIC android log Vulkan::Vulkan)
    target_compile_definitions(glfw PUBLIC GLFW_INCLUDE_NONE)
else()
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw EXCLUDE_FROM_ALL)
endif()

# --- cimgui (C API for Dear ImGui) with the GLFW and Vulkan backends ------------
set(CIMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/cimgui)
add_library(cimgui STATIC
    ${CIMGUI_DIR}/cimgui.cpp
    ${CIMGUI_DIR}/cimgui_impl.cpp
    ${CIMGUI_DIR}/imgui/imgui.cpp
    ${CIMGUI_DIR}/imgui/imgui_draw.cpp
    ${CIMGUI_DIR}/imgui/imgui_tables.cpp
    ${CIMGUI_DIR}/imgui/imgui_widgets.cpp
    ${CIMGUI_DIR}/imgui/imgui_demo.cpp
    ${CIMGUI_DIR}/imgui/backends/imgui_impl_glfw.cpp
    ${CIMGUI_DIR}/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(cimgui PUBLIC ${CIMGUI_DIR} PRIVATE ${CIMGUI_DIR}/imgui ${CIMGUI_DIR}/imgui/backends)
target_compile_definitions(cimgui
    PUBLIC  CIMGUI_USE_GLFW CIMGUI_USE_VULKAN CIMGUI_NO_EXPORT IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
    PRIVATE "IMGUI_USER_CONFIG=\"../cimconfig.h\"" "IMGUI_IMPL_API=extern \"C\"")
if(ANDROID)
    target_compile_definitions(cimgui PRIVATE IMGUI_IMPL_GLFW_DISABLE_X11 IMGUI_IMPL_GLFW_DISABLE_WAYLAND)
endif()
target_link_libraries(cimgui PUBLIC glfw Vulkan::Vulkan)

# --- meshoptimizer (zeux, MIT, external/meshoptimizer/VERSION.txt): welding, vertex cache / overdraw / fetch order,
# simplification for the imported meshes' LODs (src/mesh_build.c); its C API from C++ sources, like cimgui
file(GLOB MESHOPT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SRC})
target_include_directories(meshoptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/meshoptimizer/src)

# --- Shaders: GLSL -> SPIR-V embedded as C arrays (glslc -mfmt=c) ----------------
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin REQUIRED)
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
set(SHADER_HEADERS ${SHADER_DIR}/mat_vm.glsl ${SHADER_DIR}/mat_ops.h ${SHADER_DIR}/tl/kernel_svm_tex.h ${SHADER_DIR}/tl/kernel_svm_sim.h ${SHADER_DIR}/tl/kernel_compat.h ${SHADER_DIR}/tl/kernel_svm_types.h ${SHADER_DIR}/common.glsl ${SHADER_DIR}/gi.glsl ${SHADER_DIR}/rt.glsl ${SHADER_DIR}/atmosphere.glsl ${SHADER_DIR}/lights.glsl ${SHADER_DIR}/weather.glsl ${SHADER_DIR}/preview.glsl ${SHADER_DIR}/mat_common.glsl ${SHADER_DIR}/vsm.glsl ${SHADER_DIR}/lsm.glsl ${SHADER_DIR}/particles.glsl ${SHADER_DIR}/particles_frag.glsl ${SHADER_DIR}/water.glsl ${SHADER_DIR}/cloth.glsl ${SHADER_DIR}/denoise.glsl ${SHADER_DIR}/hair.glsl ${SHADER_DIR}/fire.glsl ${SHADER_DIR}/splat.glsl ${SHADER_DIR}/lightfn.glsl ${SHADER_DIR}/toon.glsl)
list(APPEND SHADER_HEADERS ${SHADER_DIR}/tess.glsl) # (Q17)
list(APPEND SHADER_HEADERS ${SHADER_DIR}/farview.glsl ${SHADER_DIR}/impostor.glsl) # (Q21)
list(APPEND SHADER_HEADERS ${SHADER_DIR}/mr.glsl ${SHADER_DIR}/ar.glsl) # (Q25b: mixed reality)
foreach(S mesh.vert mesh.frag fullscreen.vert sky.frag grid.frag mask.frag masked.frag composite.frag probe_trace.comp probe_update.comp
          pathtrace.comp view2.comp lightmap_bake.comp lightmap_blend.comp exposure.comp atmosphere.comp cull.comp tlas_fill.comp hiz.comp
          light_grid.comp occluder_light.comp gi_cache.comp weather.comp material_bake.comp velocity.vert velocity.frag
          bloom.comp crowd.comp preview.vert preview.frag preview_bg.frag preview_ao.frag shadow_split.comp denoise.comp den_trace.comp gtao.comp sss.comp vrs.comp ups_mv.comp decal_bin.comp alpha_hist.comp bc7.comp mip_kaiser.comp tsr.comp omm.comp velocity_crowd.vert skin.comp particle_sim.comp particle.vert
          particle.frag ribbon.vert ribbon.frag particle_mesh.vert particle_mesh.frag particle_point.vert particle_point.frag water_sim.comp water_surface.vert
          water_surface.frag rain.vert rain.frag cloth_sim.comp cloth.vert cloth.frag hair.vert hair.frag hair_sim.comp fire_sim.comp fire_render.comp splat_sort.comp splat.vert splat.frag ar_plane.vert ar_plane.frag ar_occlude.frag debug_line.vert debug_line.frag)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${S}.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -I ${SHADER_DIR} -o ${SHADER_OUT}/${S}.inc ${SHADER_DIR}/${S}
        DEPENDS ${SHADER_DIR}/${S} ${SHADER_HEADERS}
        COMMENT "glslc ${S}")
    list(APPEND SHADER_INCS ${SHADER_OUT}/${S}.inc)
endforeach()
# (Q17) Nanite Tessellation (tess.c): the dicing
foreach(S tess.comp)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${S}.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -I ${SHADER_DIR} -o ${SHADER_OUT}/${S}.inc ${SHADER_DIR}/${S}
        DEPENDS ${SHADER_DIR}/${S} ${SHADER_HEADERS}
        COMMENT "glslc ${S}")
    list(APPEND SHADER_INCS ${SHADER_OUT}/${S}.inc)
endforeach()
# (Q21) the distant view (farview.c): the terrain's horizon, the impostors' bake
foreach(S farshadow.comp impostor_bake.vert impostor_bake.frag)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${S}.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -I ${SHADER_DIR} -o ${SHADER_OUT}/${S}.inc ${SHADER_DIR}/${S}
        DEPENDS ${SHADER_DIR}/${S} ${SHADER_HEADERS}
        COMMENT "glslc ${S}")
    list(APPEND SHADER_INCS ${SHADER_OUT}/${S}.inc)
endforeach()
# (Q11) the outline pass (outline.c): the outlined actors' ids, the silhouettes and their Auto Adaptive widths
foreach(S outline_id.frag outline.comp)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${S}.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -I ${SHADER_DIR} -o ${SHADER_OUT}/${S}.inc ${SHADER_DIR}/${S}
        DEPENDS ${SHADER_DIR}/${S} ${SHADER_HEADERS}
        COMMENT "glslc ${S}")
    list(APPEND SHADER_INCS ${SHADER_OUT}/${S}.inc)
endforeach()
# mesh.frag with the ray traced shadows and reflections (GPUs with ray queries)
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_rt.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt.frag.inc ${SHADER_DIR}/mesh.frag
    DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
    COMMENT "glslc mesh.frag (RE_RT)")
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_rt.frag.inc)
# ... and both with the alpha cut and the two-sided back faces (the meshes of Masked / Two Sided materials: their own runs)
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_masked.frag.inc ${SHADER_OUT}/mesh_rt_masked.frag.inc ${SHADER_OUT}/mesh_masked_eq.frag.inc ${SHADER_OUT}/mesh_rt_masked_eq.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_MASKED -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_masked.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -DRE_MASKED -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt_masked.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_MASKED -DRE_MASKED_EQ -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_masked_eq.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -DRE_MASKED -DRE_MASKED_EQ -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt_masked_eq.frag.inc ${SHADER_DIR}/mesh.frag
    DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
    COMMENT "glslc mesh.frag (RE_MASKED)")
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_masked.frag.inc ${SHADER_OUT}/mesh_rt_masked.frag.inc ${SHADER_OUT}/mesh_masked_eq.frag.inc
     ${SHADER_OUT}/mesh_rt_masked_eq.frag.inc)
# ... the ray traced ones with the shadow terminator's fix (the triangle's vertices: barycentrics), for the imported
# meshes only (low-poly characters and props shadowed themselves in blocks; the basic shapes don't pay for it)
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_rt_t.frag.inc ${SHADER_OUT}/mesh_rt_masked_t.frag.inc ${SHADER_OUT}/mesh_rt_masked_eq_t.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -DRE_TERMINATOR -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt_t.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -DRE_TERMINATOR -DRE_MASKED -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt_masked_t.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_MASKED_EQ -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt_masked_eq_t.frag.inc ${SHADER_DIR}/mesh.frag
    DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
    COMMENT "glslc mesh.frag (RE_TERMINATOR)")
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_rt_t.frag.inc ${SHADER_OUT}/mesh_rt_masked_t.frag.inc ${SHADER_OUT}/mesh_rt_masked_eq_t.frag.inc)
# mesh.frag with the local lights' shadow maps (RE_LSM: render.c picks it while a local light has a map; the plain one
# keeps its registers without the maps' code: notas-dev/luz.md "Sombras das luzes locais por mapa")
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_lsm.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_LSM -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_lsm.frag.inc ${SHADER_DIR}/mesh.frag
    DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
    VERBATIM)
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_lsm.frag.inc)
# hair.frag (A9.3b) for the main pass's 2 and 4 targets (the split's, the denoiser's: written zero)
add_custom_command( # (2026-11-15) the cloth in the velocity pass
    OUTPUT ${SHADER_OUT}/cloth_vel.vert.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DCLOTH_VELOCITY -I ${SHADER_DIR} -o ${SHADER_OUT}/cloth_vel.vert.inc ${SHADER_DIR}/cloth.vert
    DEPENDS ${SHADER_DIR}/cloth.vert ${SHADER_HEADERS}
    COMMENT "glslc cloth.vert (velocity)")
list(APPEND SHADER_INCS ${SHADER_OUT}/cloth_vel.vert.inc)
add_custom_command( # (Q9) the soft bodies' block pass: a workgroup a soft body, its lattice in shared memory (cloth.c)
    OUTPUT ${SHADER_OUT}/cloth_soft.comp.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DSOFT_BLOCK -I ${SHADER_DIR} -o ${SHADER_OUT}/cloth_soft.comp.inc ${SHADER_DIR}/cloth_sim.comp
    DEPENDS ${SHADER_DIR}/cloth_sim.comp ${SHADER_HEADERS}
    COMMENT "glslc cloth_sim.comp (soft bodies' block pass)")
list(APPEND SHADER_INCS ${SHADER_OUT}/cloth_soft.comp.inc)
add_custom_command( # (2026-11-15) the hair in the velocity pass
    OUTPUT ${SHADER_OUT}/hair_vel.vert.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DHAIR_VELOCITY -I ${SHADER_DIR} -o ${SHADER_OUT}/hair_vel.vert.inc ${SHADER_DIR}/hair.vert
    DEPENDS ${SHADER_DIR}/hair.vert ${SHADER_HEADERS}
    COMMENT "glslc hair.vert (velocity)")
list(APPEND SHADER_INCS ${SHADER_OUT}/hair_vel.vert.inc)
add_custom_command(
    OUTPUT ${SHADER_OUT}/hair_depth.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DHAIR_DEPTH -I ${SHADER_DIR} -o ${SHADER_OUT}/hair_depth.frag.inc ${SHADER_DIR}/hair.frag
    DEPENDS ${SHADER_DIR}/hair.frag ${SHADER_HEADERS}
    COMMENT "glslc hair.frag (depth)")
list(APPEND SHADER_INCS ${SHADER_OUT}/hair_depth.frag.inc)
foreach(HO 2 4)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/hair_o${HO}.frag.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DHAIR_OUT=${HO} -I ${SHADER_DIR} -o ${SHADER_OUT}/hair_o${HO}.frag.inc ${SHADER_DIR}/hair.frag
        DEPENDS ${SHADER_DIR}/hair.frag ${SHADER_HEADERS}
        COMMENT "glslc hair.frag (${HO} targets)")
    list(APPEND SHADER_INCS ${SHADER_OUT}/hair_o${HO}.frag.inc)
endforeach()
# mesh.frag with the Principled BSDF's extra lobes (RE_EXT: coat, anisotropy, subsurface, IOR / specular tint; M1b):
# render.c picks them in the frames a visible material has them; the plain ones keep their registers (notas-dev/materiais.md)
set(EXT_VARIANTS "x:-DRE_EXT" "rt_x:-DRE_RT -DRE_EXT" "rt_t_x:-DRE_RT -DRE_TERMINATOR -DRE_EXT" "rt_x_a:-DRE_RT -DRE_EXT -DRT_ALPHA"
    "rt_t_x_a:-DRE_RT -DRE_TERMINATOR -DRE_EXT -DRT_ALPHA"
    # (2026-11-10) the masked runs' (Masked / Two Sided materials with coat, anisotropy, subsurface, IOR: hair, leaves)
    "masked_x:-DRE_MASKED -DRE_EXT" "masked_eq_x:-DRE_MASKED -DRE_MASKED_EQ -DRE_EXT"
    "rt_masked_x:-DRE_RT -DRE_MASKED -DRE_EXT" "rt_masked_eq_x:-DRE_RT -DRE_MASKED -DRE_MASKED_EQ -DRE_EXT"
    "rt_masked_t_x:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_EXT" "rt_masked_eq_t_x:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_MASKED_EQ -DRE_EXT"
    "rt_masked_x_a:-DRE_RT -DRE_MASKED -DRE_EXT -DRT_ALPHA" "rt_masked_eq_x_a:-DRE_RT -DRE_MASKED -DRE_MASKED_EQ -DRE_EXT -DRT_ALPHA"
    "rt_masked_t_x_a:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_EXT -DRT_ALPHA"
    "rt_masked_eq_t_x_a:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_MASKED_EQ -DRE_EXT -DRT_ALPHA")
foreach(V ${EXT_VARIANTS})
    string(REPLACE ":" ";" VP "${V}")
    list(GET VP 0 VNAME)
    list(GET VP 1 VDEFS)
    separate_arguments(VDEFS)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/mesh_${VNAME}.frag.inc ${SHADER_OUT}/mesh_${VNAME}_toon.frag.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}.frag.inc ${SHADER_DIR}/mesh.frag
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -DRE_TOON -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}_toon.frag.inc ${SHADER_DIR}/mesh.frag
        DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
        COMMENT "glslc mesh.frag (${VNAME}, RE_EXT; Q11: and its RE_TOON twin)")
    list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_${VNAME}.frag.inc ${SHADER_OUT}/mesh_${VNAME}_toon.frag.inc)
endforeach()
# (Q22) Nanite Tessellation's micro vertices (tess.vert; render.c's tess twins), plain and RE_EXT (the vertex color out)
add_custom_command(
    OUTPUT ${SHADER_OUT}/tess.vert.inc ${SHADER_OUT}/tess_x.vert.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -I ${SHADER_DIR} -o ${SHADER_OUT}/tess.vert.inc ${SHADER_DIR}/tess.vert
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_EXT -I ${SHADER_DIR} -o ${SHADER_OUT}/tess_x.vert.inc ${SHADER_DIR}/tess.vert
    DEPENDS ${SHADER_DIR}/tess.vert ${SHADER_HEADERS}
    COMMENT "glslc tess.vert"
    VERBATIM)
list(APPEND SHADER_INCS ${SHADER_OUT}/tess.vert.inc ${SHADER_OUT}/tess_x.vert.inc)
# mesh.vert for the RE_EXT pipelines: the vertex color out (M5; the other pipelines' vertices pay nothing)
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_x.vert.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_EXT -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_x.vert.inc ${SHADER_DIR}/mesh.vert
    DEPENDS ${SHADER_DIR}/mesh.vert ${SHADER_HEADERS}
    COMMENT "glslc mesh.vert (RE_EXT)"
    VERBATIM)
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_x.vert.inc)
# (Q11) mesh.frag with the local lights' shadow maps and the RE_EXT features together (Low / the GPUs without rays: a Toon
# material, the lobes, the wet, the Light Functions kept while a local light has a map or SSR runs)
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_lsm_x.frag.inc ${SHADER_OUT}/mesh_lsm_x_toon.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_LSM -DRE_EXT -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_lsm_x.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_LSM -DRE_EXT -DRE_TOON -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_lsm_x_toon.frag.inc ${SHADER_DIR}/mesh.frag
    DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
    COMMENT "glslc mesh.frag (RE_LSM + RE_EXT)"
    VERBATIM)
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_lsm_x.frag.inc ${SHADER_OUT}/mesh_lsm_x_toon.frag.inc)
# Translucent materials (render.c's OIT pass): weighted blended OIT, with and without the ray traced lights
add_custom_command(
    OUTPUT ${SHADER_OUT}/mesh_tl.frag.inc ${SHADER_OUT}/mesh_rt_tl.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_TRANSLUCENT -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_tl.frag.inc ${SHADER_DIR}/mesh.frag
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -DRE_TRANSLUCENT -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_rt_tl.frag.inc ${SHADER_DIR}/mesh.frag
    DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
    VERBATIM)
list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_tl.frag.inc ${SHADER_OUT}/mesh_rt_tl.frag.inc)
# ... the ray traced ones again with the shadow rays' alpha test (RT_ALPHA: the scenes that draw a Masked mesh; hair cards
# cast their strands' shadow instead of their quads'); the others don't carry its ray query
set(RT_ALPHA_VARIANTS "rt:-DRE_RT" "rt_masked:-DRE_RT -DRE_MASKED" "rt_masked_eq:-DRE_RT -DRE_MASKED -DRE_MASKED_EQ" "rt_t:-DRE_RT -DRE_TERMINATOR"
    "rt_masked_t:-DRE_RT -DRE_TERMINATOR -DRE_MASKED" "rt_masked_eq_t:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_MASKED_EQ")
foreach(V ${RT_ALPHA_VARIANTS})
    string(REPLACE ":" ";" VP "${V}")
    list(GET VP 0 VNAME)
    list(GET VP 1 VDEFS)
    separate_arguments(VDEFS)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/mesh_${VNAME}_a.frag.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -DRT_ALPHA -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}_a.frag.inc ${SHADER_DIR}/mesh.frag
        DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
        COMMENT "glslc mesh.frag (${VNAME}, RT_ALPHA)")
    list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_${VNAME}_a.frag.inc)
endforeach()
# ... and the ray traced ones that write the unified denoiser's targets (RE_DEN, A9.1d: the Photoreal styles; render.c
# den_rt makes them on a thread the first frame it runs). Apart, so the others are the very code they were
set(DEN_VARIANTS "rt:-DRE_RT" "rt_masked:-DRE_RT -DRE_MASKED" "rt_masked_eq:-DRE_RT -DRE_MASKED -DRE_MASKED_EQ" "rt_t:-DRE_RT -DRE_TERMINATOR"
    "rt_masked_t:-DRE_RT -DRE_TERMINATOR -DRE_MASKED" "rt_masked_eq_t:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_MASKED_EQ"
    "rt_x:-DRE_RT -DRE_EXT" "rt_t_x:-DRE_RT -DRE_TERMINATOR -DRE_EXT"
    "rt_masked_x:-DRE_RT -DRE_MASKED -DRE_EXT" "rt_masked_eq_x:-DRE_RT -DRE_MASKED -DRE_MASKED_EQ -DRE_EXT"
    "rt_masked_t_x:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_EXT" "rt_masked_eq_t_x:-DRE_RT -DRE_TERMINATOR -DRE_MASKED -DRE_MASKED_EQ -DRE_EXT")
foreach(V ${DEN_VARIANTS})
    string(REPLACE ":" ";" VP "${V}")
    list(GET VP 0 VNAME)
    list(GET VP 1 VDEFS)
    separate_arguments(VDEFS)
    if(VNAME MATCHES "_x$") # (Q11) the RE_EXT ones' RE_TOON twins
        add_custom_command(
            OUTPUT ${SHADER_OUT}/mesh_${VNAME}_d_toon.frag.inc ${SHADER_OUT}/mesh_${VNAME}_a_d_toon.frag.inc
            COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -DRE_DEN -DRE_TOON -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}_d_toon.frag.inc ${SHADER_DIR}/mesh.frag
            COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -DRE_DEN -DRT_ALPHA -DRE_TOON -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}_a_d_toon.frag.inc ${SHADER_DIR}/mesh.frag
            DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
            COMMENT "glslc mesh.frag (${VNAME}, RE_DEN, RE_TOON)")
        list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_${VNAME}_d_toon.frag.inc ${SHADER_OUT}/mesh_${VNAME}_a_d_toon.frag.inc)
    endif()
    add_custom_command(
        OUTPUT ${SHADER_OUT}/mesh_${VNAME}_d.frag.inc ${SHADER_OUT}/mesh_${VNAME}_a_d.frag.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -DRE_DEN -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}_d.frag.inc ${SHADER_DIR}/mesh.frag
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${VDEFS} -DRE_DEN -DRT_ALPHA -I ${SHADER_DIR} -o ${SHADER_OUT}/mesh_${VNAME}_a_d.frag.inc ${SHADER_DIR}/mesh.frag
        DEPENDS ${SHADER_DIR}/mesh.frag ${SHADER_HEADERS}
        COMMENT "glslc mesh.frag (${VNAME}, RE_DEN)")
    list(APPEND SHADER_INCS ${SHADER_OUT}/mesh_${VNAME}_d.frag.inc ${SHADER_OUT}/mesh_${VNAME}_a_d.frag.inc)
endforeach()
# the GI's rays by software (A1, sw_rt.glsl: RE_SWRT) for the GPUs without ray queries
foreach(S probe_trace occluder_light gi_cache)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${S}_sw.comp.inc
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_SWRT -I ${SHADER_DIR} -o ${SHADER_OUT}/${S}_sw.comp.inc ${SHADER_DIR}/${S}.comp
        DEPENDS ${SHADER_DIR}/${S}.comp ${SHADER_HEADERS} ${SHADER_DIR}/sw_rt.glsl
        COMMENT "glslc ${S}.comp (RE_SWRT)")
    list(APPEND SHADER_INCS ${SHADER_OUT}/${S}_sw.comp.inc)
endforeach()
# water_sim.comp with the rain's surface traced from above (GPUs with ray queries)
add_custom_command(
    OUTPUT ${SHADER_OUT}/water_sim_rt.comp.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -I ${SHADER_DIR} -o ${SHADER_OUT}/water_sim_rt.comp.inc ${SHADER_DIR}/water_sim.comp
    DEPENDS ${SHADER_DIR}/water_sim.comp ${SHADER_HEADERS}
    COMMENT "glslc water_sim.comp (RE_RT)")
list(APPEND SHADER_INCS ${SHADER_OUT}/water_sim_rt.comp.inc)
# (Q25) vrs.comp's VR foveation alone: the rings by the distance to the eye's center, nothing read (the last color's and
# the depth's descriptors not used: their layouts don't matter where VR's MSAA leaves them)
add_custom_command(
    OUTPUT ${SHADER_OUT}/vrs_fov.comp.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_VRS_FOVEATED -I ${SHADER_DIR} -o ${SHADER_OUT}/vrs_fov.comp.inc ${SHADER_DIR}/vrs.comp
    DEPENDS ${SHADER_DIR}/vrs.comp ${SHADER_HEADERS}
    COMMENT "glslc vrs.comp (RE_VRS_FOVEATED)")
list(APPEND SHADER_INCS ${SHADER_OUT}/vrs_fov.comp.inc)
# water_sim.comp's FFT passes alone (its own pipeline: Adreno's compiler crashed on them among the other passes)
add_custom_command(
    OUTPUT ${SHADER_OUT}/water_fft.comp.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_WATER_FFT -I ${SHADER_DIR} -o ${SHADER_OUT}/water_fft.comp.inc ${SHADER_DIR}/water_sim.comp
    DEPENDS ${SHADER_DIR}/water_sim.comp ${SHADER_HEADERS}
    COMMENT "glslc water_sim.comp (RE_WATER_FFT)")
list(APPEND SHADER_INCS ${SHADER_OUT}/water_fft.comp.inc)
# water_surface.frag seeing the bottom through shallow water, shore foam (GPUs with ray queries)
add_custom_command(
    OUTPUT ${SHADER_OUT}/water_surface_rt.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -I ${SHADER_DIR} -o ${SHADER_OUT}/water_surface_rt.frag.inc ${SHADER_DIR}/water_surface.frag
    DEPENDS ${SHADER_DIR}/water_surface.frag ${SHADER_HEADERS}
    COMMENT "glslc water_surface.frag (RE_RT)")
list(APPEND SHADER_INCS ${SHADER_OUT}/water_surface_rt.frag.inc)
# weather.comp with the fog's shadow rays (GPUs with ray queries)
add_custom_command(
    OUTPUT ${SHADER_OUT}/weather_rt.comp.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_RT -I ${SHADER_DIR} -o ${SHADER_OUT}/weather_rt.comp.inc ${SHADER_DIR}/weather.comp
    DEPENDS ${SHADER_DIR}/weather.comp ${SHADER_HEADERS}
    COMMENT "glslc weather.comp (RE_RT)")
list(APPEND SHADER_INCS ${SHADER_OUT}/weather_rt.comp.inc)
# composite.frag with the depth of field and motion blur gather (only the frames that have something to blur)
add_custom_command(
    OUTPUT ${SHADER_OUT}/composite_post.frag.inc
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -DRE_POST -I ${SHADER_DIR} -o ${SHADER_OUT}/composite_post.frag.inc ${SHADER_DIR}/composite.frag
    DEPENDS ${SHADER_DIR}/composite.frag ${SHADER_HEADERS}
    COMMENT "glslc composite.frag (RE_POST)")
list(APPEND SHADER_INCS ${SHADER_OUT}/composite_post.frag.inc)
add_custom_target(re_shaders DEPENDS ${SHADER_INCS})

# --- The engine core (no window, no UI): what the editor and the tests share ---
add_library(re_core STATIC
    src/ecs.c
    src/spline.c
    src/gameplay_tags.c
    src/mem.c
    src/systems.c
    src/jobs.c
    src/i18n.c
    src/i18n_table.c
    src/crashlog.c
    src/re_crypto.c
    src/re_zlib.c
    src/rpak.c
    src/trace.c
    src/plugins.c
    src/zip_read.c
    src/net_http.c
    src/io_json.c
    src/endpoints.c
    src/report_upload.c
    src/updater.c
    src/update_install.c
    src/platform_win32.c
    src/platform_files.c
    src/async.c
    src/audio.c
    src/metasound.c
    src/audio_wave.c
    src/audio_assets.c
    src/audio_dsp.c
)
target_include_directories(re_core PUBLIC src)
if(ANDROID)
    target_sources(re_core PRIVATE android/platform_android.c)
    target_link_libraries(re_core PUBLIC glfw android log aaudio jnigraphics m) # (aaudio: the mixer's output; jnigraphics: the image decoder)
else()
    target_link_libraries(re_core PUBLIC glfw shell32 ole32 comdlg32 dbghelp winhttp bcrypt avrt) # (avrt: the audio thread's Pro Audio priority)
endif()

# Generated: the commit (crash and bug reports) and endpoints.ini (embedded)
set(RE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${RE_GENERATED_DIR})
find_program(GIT git)
add_custom_target(re_commit
    COMMAND ${CMAKE_COMMAND} -DGIT=${GIT} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${RE_GENERATED_DIR}/re_commit.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_commit.cmake
    BYPRODUCTS ${RE_GENERATED_DIR}/re_commit.h)
add_custom_command(OUTPUT ${RE_GENERATED_DIR}/endpoints.ini.inc
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/data/branding/endpoints.ini -DOUT=${RE_GENERATED_DIR}/endpoints.ini.inc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/branding/endpoints.ini ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_text.cmake)
foreach(B logo:logo.png splash:splash.jpg) # the intro's logo and the loading screen (src/brand.c)
    string(REPLACE ":" ";" BP "${B}")
    list(GET BP 0 BN)
    list(GET BP 1 BF)
    add_custom_command(OUTPUT ${RE_GENERATED_DIR}/brand_${BN}.inc
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/data/branding/${BF} -DOUT=${RE_GENERATED_DIR}/brand_${BN}.inc
                -DNAME=g_brand_${BN} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_bin.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/branding/${BF} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_bin.cmake)
endforeach()
foreach(F inter_regular:Inter-Regular.ttf inter_semibold:Inter-SemiBold.ttf sora_semibold:Sora-SemiBold.ttf jetbrains_mono:JetBrainsMono-Regular.ttf) # the editor's fonts (OFL, src/theme.c)
    string(REPLACE ":" ";" FP "${F}")
    list(GET FP 0 FN)
    list(GET FP 1 FF)
    add_custom_command(OUTPUT ${RE_GENERATED_DIR}/font_${FN}.inc
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/data/fonts/${FF} -DOUT=${RE_GENERATED_DIR}/font_${FN}.inc
                -DNAME=g_font_${FN} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_bin.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/fonts/${FF} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_bin.cmake)
    list(APPEND RE_FONT_INCS ${RE_GENERATED_DIR}/font_${FN}.inc)
endforeach()
add_custom_target(re_generated DEPENDS ${RE_GENERATED_DIR}/endpoints.ini.inc ${RE_GENERATED_DIR}/brand_logo.inc ${RE_GENERATED_DIR}/brand_splash.inc ${RE_FONT_INCS})
add_dependencies(re_core re_commit re_generated)
target_include_directories(re_core PUBLIC ${RE_GENERATED_DIR})
target_compile_definitions(re_core PUBLIC RE_VERSION_STRING="${RE_VERSION_STRING}")

# The installer of the updates (runs after the editor closed) and the release tools (keys, signature)
if(NOT ANDROID)
add_executable(realengine_updater src/updater_main.c src/update_install.c src/re_crypto.c)
target_include_directories(realengine_updater PRIVATE src)
target_link_libraries(realengine_updater PRIVATE bcrypt shell32)
endif()

# --- The renderer and the levels (no UI): what the editor and the packaged game share ---
add_library(re_render STATIC
    src/render.c
    src/tess.c
    src/farview.c
    src/mobile.c
    src/render_preview.c
    src/gi.c
    src/rt_clusters.c
    src/sw_rt.c
    src/gi_bake.c
    src/gi_cache.c
    src/vsm.c
    src/lsm.c
    src/cloud_noise.c
    src/net.c
    src/net_repl.c
    src/net_udp.c
    src/steam.c
    src/online.c
    src/net_test.c
    src/net_session_test.c
    src/weather.c
    src/weather_field.c
    src/city.c
    src/gi_fallback.c
    src/water.c
    src/lens_water.c
    src/sublevels.c
    src/bp_timeline.c
    src/cloth.c
    src/softbody.c
    src/groom.c
    src/fire.c
    src/outline.c
    src/toon.c
    src/toon_actor.c
    src/river.c
    src/splat.c
    src/delight.c
    src/volume.c
    src/world_partition.c
    src/vdb.c
    src/volume_grid.c
    src/xr.c
    src/xr_mr.c
    src/ar.c
    src/ar_arcore.c
    src/cloth_mesh.c
    src/atmosphere.c
    src/lights.c
    src/vkctx.c
    src/level.c
    src/level_stream.c
    src/input.c
    src/post.c
    src/terror.c
    src/runtime_lighting.c
    src/props.c
    src/cvars.c
    src/blueprint.c
    src/material.c
    src/tex_settings.c # (2026-11-23) a texture's own settings (color space, size, compression)
    src/mat_nodes.c
    src/mat_image.c
    src/udim.c
    src/mat_examples.c
    src/bp_vm.c
    src/bp_jit.c
    src/sequencer.c
    src/demo_play.c
    src/bp_components.c
    src/physics.c
    src/scene_query.c
    src/debug_draw.c
    src/physics_shapes.c
    src/physmat.c
    src/camshake.c
    src/fpcam.c
    src/spline_world.c
    src/anim.c
    src/particles.c
    src/ikrig.c
    src/charmap.c # (2026-11-23) the Tlender's character definition, rig systems: rig_detect.c
    src/charmap_analyze.c
    src/rig_systems.c
    src/rig_detect.c
    src/level_preload.c # (2026-11-23) levels loaded in the background at a speed
    src/vehicle.c
    src/procgen.c
    src/pcg_graph.c
    src/pcg_exec.c
    src/pcg_world.c
    src/ai.c
    src/navmesh.c
    src/navmesh_build.c
    src/health.c
    src/inventory.c
    src/interaction.c
    src/vr_pawn.c
    src/vr_hand.c
    src/dialogue.c
    src/quest.c
    src/game_rules.c
    src/rules_widgets.c
    src/ability.c
    src/frame_budget.c
    src/upscale.c
    src/upscale_vendor.c
    src/decal.c
    src/hud.c
    src/crowd.c
    src/traffic.c
    src/ui_widget.c
    src/game_settings.c
    src/pause_menu.c
    src/save_progress.c
    src/save_menu.c
    src/landscape.c
    src/foliage.c
    src/proto.c
    src/tlend_import.c
    src/import_report.c
    src/driver_expr.c
    src/mesh_build.c
    src/mesh_clod.cpp
    src/heavy_test.c
    src/ik_solver.c
    src/fbx_read.c
    src/fbx_write.c
    src/project.c
    src/audio_world.c
    src/script.c
    src/script_parse.c
    src/script_lower.c
    src/bp_fiber.c
)
if(ANDROID)
    target_link_libraries(re_render PUBLIC re_core glfw Vulkan::Vulkan meshoptimizer)
else()
    target_link_libraries(re_render PUBLIC re_core glfw Vulkan::Vulkan ws2_32 meshoptimizer) # (ws2_32: the network, net_udp.c)
endif()
add_dependencies(re_render re_shaders)
target_include_directories(re_render PRIVATE ${SHADER_OUT})

# --- The vendors' upscalers (upscale_vendor.c; notas-dev/upscaling.md): tools/fetch_upscalers.ps1 downloads the SDKs into
# external/upscalers (not versioned: each one's license there). Without them the build has TSR only; with them their
# runtime DLLs go beside the editor and the game (RE_UPS_DLLS, copied below and by Build > Package Game)
set(RE_UPS ${CMAKE_CURRENT_SOURCE_DIR}/external/upscalers)
set(RE_UPS_DLLS)
set(RE_UPS_LICENSES) # (pairs: the SDK's license, its name beside the DLL)
if(WIN32 AND NOT ANDROID)
    if(EXISTS ${RE_UPS}/dlss/lib/nvsdk_ngx_d.lib AND EXISTS ${RE_UPS}/dlss/bin/nvngx_dlss.dll)
        target_compile_definitions(re_render PRIVATE RE_HAVE_DLSS)
        target_include_directories(re_render PRIVATE ${RE_UPS}/dlss/include)
        target_link_libraries(re_render PUBLIC ${RE_UPS}/dlss/lib/nvsdk_ngx_d.lib)
        list(APPEND RE_UPS_DLLS ${RE_UPS}/dlss/bin/nvngx_dlss.dll)
        list(APPEND RE_UPS_LICENSES ${RE_UPS}/dlss/LICENSE.txt nvngx_dlss_LICENSE.txt)
    endif()
    if(EXISTS ${RE_UPS}/fsr/include/ffx_api/ffx_upscale.h AND EXISTS ${RE_UPS}/fsr/bin/amd_fidelityfx_vk.dll)
        target_compile_definitions(re_render PRIVATE RE_HAVE_FSR)
        target_include_directories(re_render PRIVATE ${RE_UPS}/fsr/include)
        list(APPEND RE_UPS_DLLS ${RE_UPS}/fsr/bin/amd_fidelityfx_vk.dll)
        list(APPEND RE_UPS_LICENSES ${RE_UPS}/fsr/LICENSE.txt amd_fidelityfx_LICENSE.txt)
    endif()
    if(EXISTS ${RE_UPS}/xess/include/xess/xess_vk.h AND EXISTS ${RE_UPS}/xess/bin/libxess.dll)
        target_compile_definitions(re_render PRIVATE RE_HAVE_XESS)
        target_include_directories(re_render PRIVATE ${RE_UPS}/xess/include)
        list(APPEND RE_UPS_DLLS ${RE_UPS}/xess/bin/libxess.dll)
        list(APPEND RE_UPS_LICENSES ${RE_UPS}/xess/LICENSE.txt libxess_LICENSE.txt)
    endif()
    message(STATUS "Upscalers' runtimes: ${RE_UPS_DLLS}")
endif()

# --- Android: the packaged game only (the shared library NativeActivity loads; android/build_apk.ps1 packs the APK) ---
if(ANDROID)
    set_source_files_properties(src/net_session_test.c PROPERTIES HEADER_FILE_ONLY TRUE) # (a Windows test: it starts processes)
    # ARCore's C API header (src/ar_arcore.c loads libarcore_sdk_c.so at run time): downloaded once into the build (its
    # license is the ARCore Additional Terms of Service: not kept in the repository)
    set(RE_ARCORE_INC ${CMAKE_BINARY_DIR}/arcore)
    if(NOT EXISTS ${RE_ARCORE_INC}/arcore_c_api.h)
        file(DOWNLOAD https://raw.githubusercontent.com/google-ar/arcore-android-sdk/v1.57.0/libraries/include/arcore_c_api.h
             ${RE_ARCORE_INC}/arcore_c_api.h STATUS RE_ARCORE_DL)
        list(GET RE_ARCORE_DL 0 RE_ARCORE_RC)
        if(NOT RE_ARCORE_RC EQUAL 0)
            file(REMOVE ${RE_ARCORE_INC}/arcore_c_api.h)
            message(FATAL_ERROR "ARCore's header could not be downloaded (${RE_ARCORE_DL})")
        endif()
    endif()
    target_include_directories(re_render PRIVATE ${RE_ARCORE_INC})
    add_library(realengine_game SHARED src/game_main.c src/trace_view.c src/debug_viewer.c src/imgui_hooks.c src/brand.c src/theme.c src/ui_widget_draw.c src/inventory_widget.c src/quest_hud.c src/ui_tips.c src/ui_tips_table.c src/ui_tips_table_q5.c src/ui_tips_table_q8.c src/ui_tips_table_q11.c src/ui_tips_table_q12.c src/ui_tips_table_q13.c src/ui_tips_table_q14.c src/ui_tips_table_q15.c src/ui_tips_table_q16.c src/ui_tips_table_q18.c src/ui_tips_table_q19.c src/ui_tips_table_q17.c src/ui_tips_table_q20.c src/ui_tips_table_q21.c src/ui_tips_table_q25.c src/ui_tips_table_q23.c src/ui_tips_table_look.c src/ability_widget.c src/hud_widget.c)
    target_link_libraries(realengine_game PRIVATE re_render re_core cimgui glfw Vulkan::Vulkan android log aaudio jnigraphics m)
    target_include_directories(realengine_game PRIVATE ${SHADER_OUT})
    target_link_options(realengine_game PRIVATE -u ANativeActivity_onCreate) # (native_app_glue's entry kept)
    target_link_options(realengine_game PRIVATE -Wl,-z,max-page-size=16384) # (Android 15's 16 KB pages: Google Play requires it)
    return()
endif()

# --- The editor -------------------------------------------------------------------
add_executable(realengine WIN32
    src/main.c
    src/editor.c
    src/async_ui.c
    src/async_test.c
    src/assistant.c
    src/assistant_ui.c
    src/assistant_draw.c
    src/assistant_test.c
    src/editor_viewport.c
    src/editor_view2.c
    src/editor_levels.c
    src/import_report_ui.c
    src/bp_editor.c
    src/bp_timeline_ui.c
    src/graph_menu.c
    src/particle_editor.c
    src/ikrig_editor.c
    src/vehicle_editor.c
    src/bp_preview.c
    src/asset_viewport.c
    src/asset_viewport_test.c
    src/artist_test.c
    src/anim_editor.c
    src/ai_editor.c
    src/sequencer_ui.c
    src/movie_render.c
    src/trace_view.c
    src/plugins_ui.c
    src/graph_style.c
    src/graph_layout.c # (2026-11-23) Organize Nodes, every node editor
    src/theme_editor.c
    src/editor_dock.c
    src/editor_look.c
    src/dock_test.c
    src/plugins_test.c
    src/source_control.c
    src/input_editor.c
    src/project_ui.c
    src/package_res.c
    src/editor_construct.c
    src/editor_cost.c
    src/bp_test.c
    src/char_test.c
    src/nav_test.c
    src/constraint_test.c
    src/phys_test.c
    src/terror_test.c
    src/ingame_test.c
    src/phys_joints_test.c
    src/morph_test.c
    src/health_test.c
    src/inventory_test.c
    src/editor_q10.c
    src/editor_toon.c
    src/fpcam_test.c
    src/spline_test.c
    src/save_test.c
    src/inventory_ui.c
    src/interaction_test.c
    src/interaction_ui.c
    src/dialogue_editor.c
    src/dialogue_test.c
    src/quest_editor.c
    src/physmat_editor.c
    src/fpcam_ui.c
    src/camshake_editor.c
    src/pcg_editor.c
    src/pcg_test.c
    src/udim_test.c
    src/emissive_test.c
    src/farview_test.c
    src/ai_senses_test.c
    src/quest_test.c
    src/rules_ui.c
    src/rules_test.c
    src/ability_ui.c
    src/ability_test.c
    src/hud_test.c
    src/levels_test.c
    src/levels_q3_test.c
    src/pause_test.c
    src/umg_test.c
    src/seq_test.c
    src/audio_test.c
    src/script_test.c
    src/trace_test.c
    src/vr_hand_editor.c
    src/content.c
    src/content_create.c
    src/asset_picker.c
    src/picker_test.c
    src/ui_icons.c
    src/ui_tips.c
    src/ui_tips_table.c src/ui_tips_table_q5.c src/ui_tips_table_q8.c src/ui_tips_table_q11.c src/ui_tips_table_q12.c src/ui_tips_table_q13.c src/ui_tips_table_q14.c src/ui_tips_table_q15.c src/ui_tips_table_q16.c src/ui_tips_table_q18.c src/ui_tips_table_q19.c src/ui_tips_table_q17.c src/ui_tips_table_q20.c src/ui_tips_table_q21.c src/ui_tips_table_q25.c src/ui_tips_table_q23.c src/ui_tips_table_look.c
    src/services.c
    src/imgui_hooks.c
    src/debug_viewer.c
    src/mat_editor.c
    src/toon_ui.c
    src/asset_viewer.c
    src/brand.c
    src/ui_widget_draw.c
    src/theme.c
    src/inventory_widget.c
    src/quest_hud.c
    src/ability_widget.c
    src/hud_widget.c
    src/audio_editor.c
    src/audio_import.c
    src/script_ide.c
    src/script_tools.c
    src/gameplay_tags_ui.c
    src/bp_debug_test.c
)
target_link_libraries(realengine PRIVATE re_render re_core cimgui glfw Vulkan::Vulkan mfplat mfreadwrite mfuuid)
# (B14c) the example plugin (plugins/ExamplePlugin): its DLL, plugin.json and source in Plugins/ExamplePlugin next to the
# editor; the plugin API's header in SDK/ (New Plugin copies it beside a new plugin's source)
add_library(ExamplePlugin SHARED plugins/ExamplePlugin/Source/ExamplePlugin.c)
target_include_directories(ExamplePlugin PRIVATE src)
set_target_properties(ExamplePlugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/Plugins/ExamplePlugin ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugin_libs)
add_custom_command(TARGET ExamplePlugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/plugins/ExamplePlugin/plugin.json $<TARGET_FILE_DIR:ExamplePlugin>/plugin.json
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:ExamplePlugin>/Source
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/plugins/ExamplePlugin/Source/ExamplePlugin.c $<TARGET_FILE_DIR:ExamplePlugin>/Source/ExamplePlugin.c
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/plugin_api.h $<TARGET_FILE_DIR:ExamplePlugin>/Source/plugin_api.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/$<CONFIG>/SDK
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/plugin_api.h ${CMAKE_BINARY_DIR}/$<CONFIG>/SDK/plugin_api.h)
add_dependencies(realengine ExamplePlugin)
add_dependencies(realengine realengine_updater realengine_game)
target_include_directories(realengine PRIVATE ${SHADER_OUT})

# --- The packaged game: the level and its systems, no editor (Build > Package Game copies it) ---
# The demos' CC0 assets (data/demo: Poly Haven models, ambientCG textures; their licenses in LICENSES.txt), downloaded
# into DemoContent next to the editor (cached: after the first build nothing is fetched; offline: a warning)
add_custom_target(demo_assets ALL
    COMMAND ${CMAKE_COMMAND} -DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/data/demo/manifest.tsv -DLICENSES=${CMAKE_CURRENT_SOURCE_DIR}/data/demo/LICENSES.txt
            -DOUT=$<TARGET_FILE_DIR:realengine>/DemoContent -DCACHE=${CMAKE_BINARY_DIR}/demo_cache -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_demo_assets.cmake
    COMMENT "Demo assets (CC0)")
add_dependencies(demo_assets realengine)
add_executable(realengine_game WIN32 src/game_main.c src/trace_view.c src/debug_viewer.c src/imgui_hooks.c src/brand.c src/theme.c src/ui_widget_draw.c src/inventory_widget.c src/quest_hud.c src/ui_tips.c src/ui_tips_table.c src/ui_tips_table_q5.c src/ui_tips_table_q8.c src/ui_tips_table_q11.c src/ui_tips_table_q12.c src/ui_tips_table_q13.c src/ui_tips_table_q14.c src/ui_tips_table_q15.c src/ui_tips_table_q16.c src/ui_tips_table_q18.c src/ui_tips_table_q19.c src/ui_tips_table_q17.c src/ui_tips_table_q20.c src/ui_tips_table_q21.c src/ui_tips_table_q25.c src/ui_tips_table_q23.c src/ui_tips_table_look.c src/ability_widget.c src/hud_widget.c) # the Debug Viewer: ImGui once F10 opens it; the intro and the loading screen
target_link_libraries(realengine_game PRIVATE re_render re_core cimgui glfw Vulkan::Vulkan mfplat mfreadwrite mfuuid)
if(RE_UPS_DLLS) # (the upscalers' runtimes beside the editor and the game: upscale_vendor.c loads them from there only)
    add_custom_command(TARGET realengine_game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RE_UPS_DLLS} $<TARGET_FILE_DIR:realengine_game>)
    add_custom_command(TARGET realengine POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RE_UPS_DLLS} $<TARGET_FILE_DIR:realengine>)
    list(LENGTH RE_UPS_LICENSES n_lic)
    math(EXPR n_lic "${n_lic} / 2 - 1")
    foreach(i RANGE ${n_lic}) # (each one's license beside its DLL: what ships with them)
        math(EXPR i_src "${i} * 2")
        math(EXPR i_dst "${i} * 2 + 1")
        list(GET RE_UPS_LICENSES ${i_src} lic_src)
        list(GET RE_UPS_LICENSES ${i_dst} lic_dst)
        add_custom_command(TARGET realengine POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${lic_src} $<TARGET_FILE_DIR:realengine>/${lic_dst})
    endforeach()
endif()

# --- Tests (ctest): the ECS and the empty editor's frame rate ---------------------
enable_testing()
add_executable(re_tests tests/test_ecs.c)
target_link_libraries(re_tests PRIVATE re_core)
add_test(NAME ecs COMMAND re_tests)
add_executable(re_test_audio tests/test_audio.c) # the mixer (silent device): levels, panning, binaural, limiter, reverb, cues, concurrency, cost
target_link_libraries(re_test_audio PRIVATE re_core)
add_test(NAME audio_mixer COMMAND re_test_audio)
if(MSVC)
    target_link_options(re_test_audio PRIVATE "$<$<CONFIG:Release>:/DEBUG>")
endif()
add_executable(re_test_i18n tests/test_i18n.c)
target_link_libraries(re_test_i18n PRIVATE re_core)
add_test(NAME i18n COMMAND re_test_i18n)
add_executable(re_test_jobs tests/test_jobs.c)
target_link_libraries(re_test_jobs PRIVATE re_core)
add_test(NAME jobs COMMAND re_test_jobs)
add_executable(re_test_ui_popups tests/test_ui_popups.c src/imgui_hooks.c) # the UI's fades never hide a popup (headless ImGui)
target_link_libraries(re_test_ui_popups PRIVATE cimgui)
add_test(NAME ui_popups COMMAND re_test_ui_popups)
add_executable(re_test_ui_tips tests/test_ui_tips.c src/ui_tips.c src/ui_tips_table.c src/ui_tips_table_q5.c src/ui_tips_table_q8.c src/ui_tips_table_q11.c src/ui_tips_table_q12.c src/ui_tips_table_q13.c src/ui_tips_table_q14.c src/ui_tips_table_q15.c src/ui_tips_table_q16.c src/ui_tips_table_q18.c src/ui_tips_table_q19.c src/ui_tips_table_q17.c src/ui_tips_table_q20.c src/ui_tips_table_q21.c src/ui_tips_table_q25.c src/ui_tips_table_q23.c src/ui_tips_table_look.c src/imgui_hooks.c) # the descriptions: table, lookup, shown beside the item
target_include_directories(re_test_ui_tips PRIVATE src)
target_link_libraries(re_test_ui_tips PRIVATE re_core cimgui)
add_test(NAME ui_tips COMMAND re_test_ui_tips)
# The themes (Q1, notas-dev/temas.md): the ten built-in ones, contrast, .rtheme, editing, the cost of a color
add_executable(re_test_theme tests/test_theme.c src/theme.c src/ui_tips.c src/ui_tips_table.c src/ui_tips_table_q5.c src/ui_tips_table_q8.c src/ui_tips_table_q11.c src/ui_tips_table_q12.c src/ui_tips_table_q13.c src/ui_tips_table_q14.c src/ui_tips_table_q15.c src/ui_tips_table_q16.c src/ui_tips_table_q18.c src/ui_tips_table_q19.c src/ui_tips_table_q17.c src/ui_tips_table_q20.c src/ui_tips_table_q21.c src/ui_tips_table_q25.c src/ui_tips_table_q23.c src/ui_tips_table_look.c src/imgui_hooks.c)
target_include_directories(re_test_theme PRIVATE src)
target_link_libraries(re_test_theme PRIVATE re_core cimgui)
add_test(NAME theme COMMAND re_test_theme)
set_tests_properties(theme PROPERTIES PASS_REGULAR_EXPRESSION "theme: OK" FAIL_REGULAR_EXPRESSION "FAIL")
# The lema's ideal: the editor with its default level (vsync off) above 350 fps for 3 s, or the test fails.
add_test(NAME editor_fps COMMAND realengine --bench 3 --min-fps 200)
# The modular windows (Q2, notas-dev/temas-e-janelas.md): a window dragged onto every docking target (a panel's center
# and sides, the outer edges, the gaps between panels) docks there; a maximized window of its own closed by its X, by
# the system and by a close that reached the main window: it closes, the engine keeps running
add_test(NAME dock_targets COMMAND realengine --validation --dock-test)
# (2026-11-20, crashes #143-146) the editor closed as by its X, nothing automated: the preferences and the layout written
# (an APPDATA of its own), the layout before ImGui's context goes (it crashed every normal close since the 0.43)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/editor_close_appdata)
add_test(NAME editor_close_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/editor_close_appdata/RealEngine)
set_tests_properties(editor_close_clean PROPERTIES FIXTURES_SETUP editor_close_dir)
add_test(NAME editor_close COMMAND realengine --validation --no-intro --scene house --quit-after 90)
add_test(NAME intro_window COMMAND realengine --intro --bench 2) # (2026-11-23) the intro in a transparent window of its own (brand_intro_win.inc)
set_tests_properties(intro_window PROPERTIES PASS_REGULAR_EXPRESSION "intro: [1-9][0-9]+ frames" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(editor_close PROPERTIES FIXTURES_REQUIRED editor_close_dir ENVIRONMENT "APPDATA=${CMAKE_BINARY_DIR}/editor_close_appdata"
                     PASS_REGULAR_EXPRESSION "Editor closed cleanly" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" TIMEOUT 180)
add_test(NAME look_migration COMMAND realengine --validation --bench 2 --look-test ${CMAKE_BINARY_DIR}/look_test) # (Q26) a 0.43 editor.ini: the new look once, the old one back
set_tests_properties(look_migration PROPERTIES PASS_REGULAR_EXPRESSION "Look migration test OK" FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 120)
# (Q5, notas-dev/icones-e-tooltips.md) the icon sheet: every icon at 16, 24 and 32 px in the theme's colors, into a PNG
add_test(NAME icon_sheet COMMAND realengine --validation --bench 2 --icon-sheet --shot ${CMAKE_BINARY_DIR}/icon_sheet.png)
set_tests_properties(icon_sheet PROPERTIES PASS_REGULAR_EXPRESSION "Screenshot written: [^
]*icon_sheet.png" FAIL_REGULAR_EXPRESSION "FAILED|IM_ASSERT" TIMEOUT 120)
set_tests_properties(dock_targets PROPERTIES PASS_REGULAR_EXPRESSION "Dock test OK" FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 180)
add_test(NAME gpu_marks COMMAND realengine --validation --scene cloth --bench 3 --check-gpu-marks) # where a lost GPU stopped: every pass marked and finished (notas-dev/servicos.md)
# The orthographic views (sugestão #66): the default level from above and from the front, validated (Vulkan's layers:
# the orthographic projection, the grid on the facing plane, the cull's LOD and Hi-Z without perspective), above 250 fps
add_test(NAME editor_ortho COMMAND realengine --validation --ortho top --bench 2 --min-fps 200)
add_test(NAME editor_ortho_front COMMAND realengine --validation --ortho front --bench 2 --min-fps 200)
# The object cost view (sugestão #65): the Debug Viewer's costs per actor colored in the 3D view, validated, > 250 fps
add_test(NAME object_cost COMMAND realengine --validation --view objcost --bench 2 --min-fps 200)
set_tests_properties(object_cost PROPERTIES PASS_REGULAR_EXPRESSION "Object cost: [1-9]")
# The surface cache (notas-dev/gi.md): a closed house in the dark lit by a spot light on a wall; validated, above 350 fps,
# and still: the flicker probe (the mean change of the scene between frames) under 0.5% (the probes alone: ~4%)
add_test(NAME gi_house COMMAND realengine --validation --scene house --bench 3 --min-fps 200 --max-flicker 0.12) # (alone 0.085-0.091%, in the whole suite up to 0.110%: notas-dev/luz.md)
add_test(NAME gi_house_thin COMMAND realengine --validation --scene house_thin --bench 2 --min-fps 200 --max-flicker 0.12) # (as gi_house)
add_test(NAME gi_house_movable COMMAND realengine --validation --scene house_movable --bench 2 --min-fps 200 --max-flicker 0.13) # (alone 0.095-0.100%, in the whole suite 0.129%: notas-dev/luz.md)
# The area lights' shadows filtered (render.md, the shadow split): the house's spot and point light as 30 cm spheres,
# validated (the main pass's second target, shadow_split.comp), no flicker
add_test(NAME shadow_filter COMMAND realengine --validation --scene house --source-radius 30 --bench 3 --min-fps 200 --max-flicker 0.12) # (as gi_house)
add_test(NAME spec_leak COMMAND realengine --validation --scene spec_leak --scalability 1 --bench 2 --min-fps 200 --max-scene-lum 30) # a lamp behind a 5 cm wall: the dark side stays dark on Medium (was 3174 nits: no local shadows; notas-dev/luz.md "Especular vazando")
add_test(NAME spec_leak_epic COMMAND realengine --validation --scene spec_leak --scalability 3 --bench 2 --min-fps 200 --max-scene-lum 5) # (the same on Epic: 1.2-1.7 nits at the walls' junctions, the surface cache)
add_test(NAME spec_leak_low COMMAND realengine --validation --scene spec_leak --scalability 0 --bench 2 --min-fps 200 --max-scene-lum 30) # (Low: the local lights' shadow maps, lsm.c; was 3037 nits: no local shadows)
add_test(NAME spec_leak_nort COMMAND realengine --validation --no-rt --scene spec_leak --scalability 1 --bench 2 --min-fps 200 --max-scene-lum 30) # (no ray tracing forced: the maps in Medium; was 3174)
# Many lights with shadows (notas-dev/luz.md "Muitas luzes com sombra"): 10 k soft lights casting, at night, every one
# shadowed by the sampling: ~89 fps at Epic (the old path, 8 a pixel traced and the rest unshadowed: 31), validated
add_test(NAME many_lights COMMAND realengine --validation --stress-objects 10000 --stress-lights 10000 --stress-cast 1 --sky 6
         --source-radius 20 --scalability 3 --bench 3 --min-fps 70 --max-flicker 0.25)
# ... in Photoreal: ReSTIR DI (A9.1a, mesh.frag RE_DEN: the temporal reservoirs in the denoiser's guide), validated
add_test(NAME many_lights_restir COMMAND realengine --validation --stress-objects 10000 --stress-lights 10000 --stress-cast 1 --sky 6
         --source-radius 20 --scalability 3 --render-style 2 --bench 3 --min-fps 35 --max-flicker 0.3)
set_tests_properties(many_lights_restir PROPERTIES PASS_REGULAR_EXPRESSION "Denoiser: [0-9]" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# The weather (notas-dev/luz.md): the sunset preset (clouds lit low, their shadow, the fog's froxels), validated, > 350 fps
add_test(NAME weather_sky COMMAND realengine --validation --sky 2 --bench 2 --min-fps 200) # (post processing ~0.13 ms, notas-dev/render.md; the screen's clouds ~0.4 ms, notas-dev/luz.md)
# Online (notas-dev/online.md): the protocol under 150 ms RTT, jitter, 10% loss and duplicates (8 runs, virtual time);
# a LAN session between two processes (the host walks; the client must see it smooth, own its pawn) through 100 ms,
# +-20 ms and 5% loss each way
add_test(NAME net_protocol COMMAND realengine --net-test)
add_test(NAME ar COMMAND realengine --ar-test) # light and color matching from a camera picture: white balance, the main light, the ambient (notas-dev/xr.md)
add_test(NAME cloth_mesh_graph COMMAND realengine --cloth-mesh-test) # a mesh's cloth: welded seam, springs, colors, pins (cloth_mesh.c)
# (Q9) soft bodies (softbody.c, notas-dev/soft-body.md): the lattice of a sphere; a jelly and a rubber ball keep their volume
# and never explode; a sofa of 3 cushions (someone sitting, a Mannequin jumping) under 0.5 ms of GPU; a 25 kg box on a
# cushion sinks in and stops. Their projects made anew each run (the scenes write their meshes into them)
add_test(NAME softbody_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/test_projects/SoftVolume ${CMAKE_BINARY_DIR}/test_projects/SoftSofa ${CMAKE_BINARY_DIR}/test_projects/SoftRigid)
set_tests_properties(softbody_clean PROPERTIES FIXTURES_SETUP soft_projects)
add_test(NAME softbody_lattice COMMAND realengine --softbody-test)
set_tests_properties(softbody_lattice PROPERTIES PASS_REGULAR_EXPRESSION "Soft body lattice: " FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME softbody_volume COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SoftVolume --template 0 --scene softbody --play --bench 4)
set_tests_properties(softbody_volume PROPERTIES FIXTURES_REQUIRED soft_projects PASS_REGULAR_EXPRESSION "Soft body rubber ball: volume" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME softbody_sofa COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SoftSofa --template 0 --scene sofa --play --bench 4 --gpu-passes --min-fps 200)
set_tests_properties(softbody_sofa PROPERTIES FIXTURES_REQUIRED soft_projects PASS_REGULAR_EXPRESSION "Soft body sofa: GPU" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME softbody_rigid COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SoftRigid --template 0 --scene softbody_rigid --play --bench 5)
set_tests_properties(softbody_rigid PROPERTIES FIXTURES_REQUIRED soft_projects PASS_REGULAR_EXPRESSION "Soft body rigid: the box" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME cloth_mesh COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/ClothMesh --template 0 --scene cloth_mesh --bench 4 --min-fps 200)
set_tests_properties(cloth_mesh PROPERTIES PASS_REGULAR_EXPRESSION "Cloth: 2 cloths" FAIL_REGULAR_EXPRESSION "FAIL") # a skirt (an imported mesh) on a walking mannequin; another made by Make Cloth from Selected
add_test(NAME arcore COMMAND realengine --arcore-test) # ARCore's simulated session: its floor, the triangles that receive the shadows, a hit, the picture's YUV (notas-dev/xr.md)
add_test(NAME water COMMAND realengine --water-test) # the ocean's spectrum (its waves' height grows with the wind) and the heights the boats feel
add_test(NAME rain_scene COMMAND realengine --validation --scene rain --bench 3 --min-fps 200) # a storm: drops, splashes, the shallow water, wet surfaces (notas-dev/agua.md)
add_test(NAME rain_lens COMMAND realengine --validation --scene rain --bench 3 --min-fps 200) # (2026-11-15) the rain with its shading styles' defaults and the water on the lens (notas-dev/agua.md)
set_tests_properties(rain_lens PROPERTIES ENVIRONMENT "REALENGINE_RAIN_LENS=1.5")
add_test(NAME rain_marks COMMAND realengine --validation --scene rain --bench 4 --min-fps 200) # (2026-11-15) a Walker soaked in the rain walking in under the roof: its wet prints and drips on the dry floor, the drops on it
set_tests_properties(rain_marks PROPERTIES ENVIRONMENT "REALENGINE_WALKER_SHELTER=1" PASS_REGULAR_EXPRESSION "[1-9][0-9]* footprints, 1 wet actors .max (0.[5-9][0-9]|1.00)" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME lake_scene COMMAND realengine --validation --scene lake --bench 3 --min-fps 200) # a shallow lake: the bottom through the water, shore foam (ray queries)
add_test(NAME snow_scene COMMAND realengine --validation --scene snow --bench 3 --min-fps 200) # snow: flakes, the cover gathering (dry under the roof), beyond the simulated window too; a Walker's prints
set_tests_properties(snow_scene PROPERTIES PASS_REGULAR_EXPRESSION "lakes, [1-9][0-9]* footprints" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME ocean_scene COMMAND realengine --validation --scene ocean --play --bench 3 --min-fps 200) # the FFT ocean, a boat on it, its wake
add_test(NAME cloth_scene COMMAND realengine --validation --scene cloth --bench 3 --min-fps 200) # 12 cloths (34 thousand vertices) by Vertex Block Descent: a flag, draping, a tear (notas-dev/tecido.md)
add_test(NAME vehicle_from_mesh COMMAND realengine --vehicle-mesh-test --bench 1) # an "imported" car (body + 4 named wheels) made into a vehicle: wheels found and measured, it rests and drives
add_test(NAME update_download COMMAND realengine --update-test) # the real latest release by the API path (github.com's download path gave 504), signature, unpack (needs the internet)
set_tests_properties(update_download PROPERTIES LABELS network)
set_tests_properties(net_protocol PROPERTIES PASS_REGULAR_EXPRESSION "Net test: OK")
# the hang watchdog: a frame frozen 7.5 s, the report must name the frozen function (the 0.17 "crash" opening a project was a freeze)
add_test(NAME hang_watchdog COMMAND realengine --hang-test --bench 12)
set_tests_properties(hang_watchdog PROPERTIES PASS_REGULAR_EXPRESSION "Hang watchdog: OK")
add_test(NAME net_session COMMAND realengine --bp-test --bench 12 --net-host-test --net-emu 100,20,5)
# (B13) Play with 2 players: this editor hosts, a second editor window opens the saved level and joins (closed with it)
add_test(NAME pie_multiplayer COMMAND realengine --scene landscape --play --pie-players 2 --bench 10)
set_tests_properties(pie_multiplayer PROPERTIES PASS_REGULAR_EXPRESSION "PIE multiplayer: 2 of 2" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(net_session PROPERTIES PASS_REGULAR_EXPRESSION "Net session: OK" FAIL_REGULAR_EXPRESSION "FAIL")
# the advanced sessions (notas-dev/online.md): password, property filters, Update / Start Session seen by the client, a ban
add_test(NAME net_session_advanced COMMAND realengine --bp-test --bench 16 --net-host-test --net-adv)
set_tests_properties(net_session_advanced PROPERTIES PASS_REGULAR_EXPRESSION "Net session: OK" FAIL_REGULAR_EXPRESSION "FAIL")
# Blueprints (notas-dev/blueprints.md): assets made, compiled and played in the editor, the world checked, validated
add_test(NAME blueprints COMMAND realengine --validation --bp-test --bp-package ${CMAKE_BINARY_DIR}/test_game/bp --bench 2 --min-fps 200) # (a walking camera: motion blur ~0.3 ms more)
# The Construction Script in the editor (sugestão #64): shown turned, the move kept, authored again before a save
add_test(NAME construction_editor COMMAND realengine --validation --cs-test --bench 1)
# A skeletal mesh moved in place as the gizmo moves it (relatório #114): its parts follow on the next update
# the particles (notas-dev/particulas.md): births and stacks; the effects' scene (validation); 5 million in the view
add_test(NAME particles_selftest COMMAND realengine --particles-test)
# (A4b) the sun's contact shadows in screen space: on Low (no ray shades the sun there), the default level, no errors
add_test(NAME contact_shadows COMMAND realengine --validation --scalability 0 --bench 2 --min-fps 200)
set_tests_properties(contact_shadows PROPERTIES PASS_REGULAR_EXPRESSION "Contact shadows: on" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (A9.4) the Post Process Volume's Color Grading LUT: a .cube read, uploaded and sampled by the composite (no errors)
add_test(NAME color_grading_lut COMMAND realengine --validation --lut-test --bench 2)
# (Q13) the horror kit (notas-dev/terror.md): the filters deterministic, each one's look, the letterbox, the stack's cost
# (< 0.3 ms at 1440p); the horror exposure keeps a dark room dark; a light on Lighting Channel 1 doesn't leak onto channel 0
add_test(NAME terror_filters COMMAND realengine --validation --scene terror_filters --bench 12)
add_test(NAME terror_exposure COMMAND realengine --validation --scene terror_exposure --bench 9)
add_test(NAME terror_channels COMMAND realengine --validation --scene terror_channels --bench 7)
set_tests_properties(terror_filters terror_exposure terror_channels PROPERTIES PASS_REGULAR_EXPRESSION "Horror kit test: PASS" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(color_grading_lut PROPERTIES PASS_REGULAR_EXPRESSION "Color grading LUT /Content/LUTTest/Invert.cube: 17\\^3" FAIL_REGULAR_EXPRESSION "FAIL|VUID|unreadable")
# (A9.4) a Cine Camera as the player's view: its manual exposure (ISO, shutter, aperture), its lens's field of view,
# the physical depth of field with a 6 blade iris and the shutter's motion blur (no errors)
add_test(NAME cine_camera COMMAND realengine --validation --cine-cam-test --bench 2)
set_tests_properties(cine_camera PROPERTIES PASS_REGULAR_EXPRESSION "Cine camera: EV100" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (A4c) screen space reflections on Medium (no traced reflections there): the default level's floor a mirror, no errors
add_test(NAME ssr COMMAND realengine --validation --ssr-test --scalability 1 --bench 2 --min-fps 200)
set_tests_properties(ssr PROPERTIES PASS_REGULAR_EXPRESSION "SSR test: the floor a mirror \\(set\\)" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (A9.7c) async compute: the particles' simulation on the compute queue, the frame split before the main pass (no errors,
# the same particles drawn)
add_test(NAME async_compute COMMAND realengine --validation --scene particles --bench 3)
set_tests_properties(async_compute PROPERTIES ENVIRONMENT "REALENGINE_ASYNC_COMPUTE=1" PASS_REGULAR_EXPRESSION "Particles: 11 emitters.*drawn [1-9][0-9]+ sprites" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME source_control COMMAND realengine --sc-test) # (B14) Git: new, submitted, changed (a temporary repository)
# (B14c) Plugins: the example plugin's nodes (pure, callable, vectors) called by a script in Play, its tick, the crash
# guard; New Plugin's template built by its build.bat (Visual Studio's compiler), loaded and called
add_test(NAME plugins COMMAND realengine --validation --plugin-test --bench 3)
set_tests_properties(plugins PROPERTIES ENVIRONMENT "REALENGINE_PLUGINS_ENABLE=ExamplePlugin;REALENGINE_PLUGIN_CRASH_TEST=1"
                     PASS_REGULAR_EXPRESSION "Plugins OK" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME plugin_template COMMAND realengine --plugin-new-test ${CMAKE_BINARY_DIR}/plugin_template)
set_tests_properties(plugin_template PROPERTIES PASS_REGULAR_EXPRESSION "TestPlugin Remap.*ok" FAIL_REGULAR_EXPRESSION "FAIL")
# (B14b) Trace: a crowd's frames recorded (the game thread's scopes, the workers, the GPU), the file read back; then the
# Trace window drawing it for a whole bench
add_test(NAME trace_record COMMAND realengine --validation --scene crowd2000 --play --bench 4 --trace ${CMAKE_BINARY_DIR}/trace_test.json)
set_tests_properties(trace_record PROPERTIES FIXTURES_SETUP trace_file PASS_REGULAR_EXPRESSION "Trace: [0-9]+ events, [0-9]+ frames, [0-9]+ tracks.*workers [1-9][0-9]*, GPU [1-9]" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME trace_window COMMAND realengine --validation --bench 2 --trace-open ${CMAKE_BINARY_DIR}/trace_test.json)
set_tests_properties(trace_window PROPERTIES FIXTURES_REQUIRED trace_file PASS_REGULAR_EXPRESSION "Trace window: opened" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(source_control PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
# IK Rig and Physics Asset (notas-dev/ikrig.md): a chain of 3 reaches its goal, a cone limit holds, a ragdoll falls and rests, its joints held
add_test(NAME ikphys_selftest COMMAND realengine --ik-test)
set_tests_properties(ikphys_selftest PROPERTIES PASS_REGULAR_EXPRESSION "ik/phys self-test: ok" FAIL_REGULAR_EXPRESSION "FAIL")
# the ragdolls played (notas-dev/ikrig.md): 6 Mannequins fall onto the floor and a step and rest there, 3 hang their arms
# from spine_03, 3 reach spheres by an IK Rig; validation clean, above the target
add_test(NAME ikphys_ragdoll COMMAND realengine --validation --scene ragdoll --play --bench 6 --min-fps 200)
# (2026-11-16) 50 Mannequins as Active Ragdolls, a hit every half second: they sway, fall and get up; 200 fps (notas-dev/ikrig.md)
add_test(NAME active_ragdoll COMMAND realengine --validation --scene active_ragdoll --play --bench 10 --min-fps 200)
set_tests_properties(active_ragdoll PROPERTIES PASS_REGULAR_EXPRESSION "Active ragdolls: " FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ikphys_ragdoll PROPERTIES PASS_REGULAR_EXPRESSION "IK/Physics: 3 rigs, 9 simulated" FAIL_REGULAR_EXPRESSION "FAIL")
# (2026-11-19) the same ragdolls on a busy PC's frames (2.5 to 40 ms, uneven: REALENGINE_RAGDOLL_DT=rand): they rest all
# the same (fixed substeps; with dt / 8 they flailed at 20 m/s whenever the suite ran beside other work)
# (2026-11-19, notas-dev/editor.md "Travadas ao abrir") the start without our pipeline cache: the main pass's pipelines
# compiled by the prewarm on threads and taken by the first frame (none compiled in it: its frame under 400 ms; the old
# start compiled them one by one there), and the same under the validation layers (the taken ones match their passes)
add_test(NAME start_pipelines COMMAND realengine --no-intro --bench 4)
set_tests_properties(start_pipelines PROPERTIES ENVIRONMENT "REALENGINE_PIPELINE_CACHE=off;REALENGINE_MAX_START_FRAME_MS=400" PASS_REGULAR_EXPRESSION "Prewarm: [0-9]+ of [0-9]+ pipelines ready" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME start_pipelines_validation COMMAND realengine --validation --no-intro --bench 3)
set_tests_properties(start_pipelines_validation PROPERTIES ENVIRONMENT "REALENGINE_PIPELINE_CACHE=off" PASS_REGULAR_EXPRESSION "Editor start: longest frame" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (2026-11-19, notas-dev/traces.md) the scene queries: the shapes against a brute force, multi, 200 000 a batch; the
# trace nodes in a Blueprint over a floor and a cube (exact distances), drawn; 200 000 traces a frame while playing
add_test(NAME scene_queries COMMAND realengine --sq-test)
set_tests_properties(scene_queries PROPERTIES PASS_REGULAR_EXPRESSION "sq-test: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME trace_nodes COMMAND realengine --validation --trace-test --bench 3)
set_tests_properties(trace_nodes PROPERTIES PASS_REGULAR_EXPRESSION "Trace test: .*: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME trace_bench COMMAND realengine --scene house --play --trace-bench 200000 --bench 4 --min-fps 60)
set_tests_properties(trace_bench PROPERTIES PASS_REGULAR_EXPRESSION "Trace bench: 200000 traces a frame" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME ikphys_ragdoll_jitter COMMAND realengine --validation --scene ragdoll --play --bench 6)
set_tests_properties(ikphys_ragdoll_jitter PROPERTIES ENVIRONMENT "REALENGINE_RAGDOLL_DT=rand" PASS_REGULAR_EXPRESSION "IK/Physics: 3 rigs, 9 simulated" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q10, notas-dev/ikrig.md) the realistic ragdoll: anatomical limits held through 5 falls, two bodies never through
# each other; the active ragdoll braces on its hands and lies face down or on its back, an arm alone; dynamic bones
# (an earring and a tail) swing, settle and keep out of the body; a sword on a socket and an actor on a bone follow
# the pose exactly; a skirt and a cape on a running Mannequin never go through its Physics Asset's bodies
add_test(NAME ragdoll_limits COMMAND realengine --ik-test ragdoll_limits)
set_tests_properties(ragdoll_limits PROPERTIES PASS_REGULAR_EXPRESSION "ragdoll_limits: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME ragdoll_active COMMAND realengine --ik-test ragdoll_active)
# (Q27.5) GTA 6's kind of reactions: a limp, a hand on the wound, writhing before getting up, flailing in a high fall; the budget
add_test(NAME ragdoll_wounds COMMAND realengine --ik-test ragdoll_wounds)
set_tests_properties(ragdoll_wounds PROPERTIES PASS_REGULAR_EXPRESSION "ragdoll_wounds: ok" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ragdoll_active PROPERTIES PASS_REGULAR_EXPRESSION "ragdoll_active: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME dyn_bones COMMAND realengine --ik-test dyn_bones)
set_tests_properties(dyn_bones PROPERTIES PASS_REGULAR_EXPRESSION "dyn_bones: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME socket_offset COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SocketOffset --template 0 --scene socket_offset --play --bench 4)
set_tests_properties(socket_offset PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "socket_offset: [0-9]+ frames" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q10 leftover, 2026-11-19) the Active Ragdoll gets up by the real get-up clips (anim_getup.inc): four falls (forward,
# backward...), each lying face down plays AM_GetUp_Front, on its back AM_GetUp_Back (both seen); no jump where the ragdoll
# hands over to the montage, the soles above the floor (the clips alone too), standing at the end where it lay, facing
# the right way
add_test(NAME getup_q10 COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/GetUp --template 0 --scene getup_q10 --play --bench 40)
set_tests_properties(getup_q10 PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "getup_q10: knocked backward" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q12) the First Person Camera (notas-dev/camera-primeira-pessoa.md): its pure step and the shakes (no window), the template's player in Play
foreach(t fp_bob_sync camshake_determinism fp_autofocus fp_land_recoil)
    add_test(NAME ${t} COMMAND realengine --fpcam-test ${t})
    set_tests_properties(${t} PROPERTIES PASS_REGULAR_EXPRESSION "${t}: ok" FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
add_test(NAME fps_camera COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/FpsCamera --template 2 --scene fps_camera --play --bench 10)
set_tests_properties(fps_camera PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "fps_camera: [0-9]+ frames" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q14) splines and cables (notas-dev/splines.md): the curve (lengths, keys, types, frames), the rivers' and rails' curve
# as before and an old level's Rail, the XPBD cable (catenary, stretch, a free end, a whipped wire, a box, the cost); the
# scene: a road and a pipe bent on the GPU, a fence of instances, four cables (poles, a turning arm, a ribbon in the wind,
# over a crate) in Play
foreach(t spline_eval spline_rail_river cable_hang)
    add_test(NAME ${t} COMMAND realengine --spline-test ${t})
    set_tests_properties(${t} PROPERTIES PASS_REGULAR_EXPRESSION "${t}: ok" FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
add_test(NAME splines_scene COMMAND realengine --validation --scene splines --play --bench 6 --min-fps 200)
set_tests_properties(splines_scene PROPERTIES PASS_REGULAR_EXPRESSION "splines: [0-9]+ frames" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME cloth_on_body COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/ClothOnBody --template 0 --scene cloth_on_body --play --bench 8)
set_tests_properties(cloth_on_body PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_CLOTH_READBACK=1" PASS_REGULAR_EXPRESSION "cloth_on_body: the cape at a run" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# Vehicles (notas-dev/veiculos.md): a car (springs, gears, brakes, a door), a tank, a motorcycle, a boat on water, an
# airplane's takeoff, a helicopter's hover and a train with its wagon, on a flat floor; then all of them in a level
# Procedural worlds (notas-dev/procedural.md): the noise far out, the biomes, a facility's rooms; the terrain streamed
# over 5 km with the origin moved back; a facility of 30 rooms
add_test(NAME procgen_selftest COMMAND realengine --procgen-test)
set_tests_properties(procgen_selftest PROPERTIES PASS_REGULAR_EXPRESSION "procgen self-test: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME procgen_scene COMMAND realengine --validation --scene procgen --play --bench 9 --min-fps 60) # (a stress: 250 m/s with validation)
set_tests_properties(procgen_scene PROPERTIES PASS_REGULAR_EXPRESSION "Procedural world: traveled" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q15) PCG (notas-dev/pcg.md): the self-test (determinism by the seed, cells, 100 000 points, the files), a forest on a
# Landscape in the editor and in PIE, a partitioned forest streamed while the player crosses 3 km (no hitch)
add_test(NAME pcg_selftest COMMAND realengine --pcg-test)
set_tests_properties(pcg_selftest PROPERTIES PASS_REGULAR_EXPRESSION "PCG self-test: OK" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME pcg_forest COMMAND realengine --validation --scene pcg_forest --bench 14 --min-fps 120)
set_tests_properties(pcg_forest PROPERTIES PASS_REGULAR_EXPRESSION "seed 8 made" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME pcg_stream COMMAND realengine --validation --scene pcg_stream --play --bench 18 --min-fps 150)
set_tests_properties(pcg_stream PROPERTIES PASS_REGULAR_EXPRESSION "PCG stream: frames" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# World Partition (B1, notas-dev/mundos-grandes.md): 4 x 4 km, 6000 props, the player crossing it at 400 m/s, the Data
# Layers switched, the HLOD; no hitch (frames over twice the median under 1 %) and the fps margin
add_test(NAME world_partition COMMAND realengine --validation --scene wp --play --bench 10 --min-fps 200)
set_tests_properties(world_partition PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# (Q27.7) a car's 250 km/h through it: the cells ahead streamed by when it gets there, none in sight still loading, <= 0.2 % hitches
add_test(NAME wp_fast COMMAND realengine --validation --scene wp_fast --play --bench 10 --min-fps 200)
set_tests_properties(wp_fast PROPERTIES PASS_REGULAR_EXPRESSION "World Partition: at 250 km/h" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# Cinematics (B9, notas-dev/sequencer.md): a falling cube recorded by the Take Recorder, its take (+ a camera cut and a
# fade from black) rendered by the Movie Render Queue at 24 fps x 4 temporal samples; every frame written, the fade, the motion
add_test(NAME cinematics COMMAND realengine --cine-test ${CMAKE_BINARY_DIR}/test_movie --bench 40)
set_tests_properties(cinematics PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME procgen_steady COMMAND realengine --scene procgen --play --bench 5 --min-fps 200) # (standing: the streamed world's steady cost)
set_tests_properties(procgen_steady PROPERTIES ENVIRONMENT "REALENGINE_PROCGEN_STILL=1" FAIL_REGULAR_EXPRESSION "FAIL: [0-9]+ fps")
add_test(NAME procgen_facility COMMAND realengine --validation --scene facility --play --bench 3 --min-fps 200)
set_tests_properties(procgen_facility PROPERTIES PASS_REGULAR_EXPRESSION "Procedural facility: 30 rooms" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME mobile_selftest COMMAND realengine --mobile-test)
add_test(NAME mr_selftest COMMAND realengine --mr-test) # (Q25b) mixed reality: the stand-in room, its actors (collide, never drawn), traces onto the floor / table / wall, the room following the play space, the level turned to the walls, a Passthrough Window, the simulated real depth against a trace, the level's floor hidden
set_tests_properties(mr_selftest PROPERTIES PASS_REGULAR_EXPRESSION "MR self-test: 0 failures" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME vr_selftest COMMAND realengine --vr-test) # (Q25) the VR pawn: play space, snap / smooth turns, teleport onto a floor and not into a wall, walking, grab / throw / socket, the laser on an Interactable and on the menu panel, tracked hands
# (2026-11-20) the VR base hand: both FBX written, read back by the engine's importer (26 bones, the skin, the 7 poses),
# the bind and the poses where the base hand has them, the solver (tracked joints anywhere, the controller's fist)
add_test(NAME vr_base_hand COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/VRHand --template 9 --vr-hand-test --save ${CMAKE_BINARY_DIR}/test_projects/VRHand/Content/VRRoom.rlevel --bench 1)
set_tests_properties(vr_base_hand PROPERTIES FIXTURES_REQUIRED projects FIXTURES_SETUP vr_hand_saved PASS_REGULAR_EXPRESSION "VR base hand: exported and imported in [0-9]+ ms, OK" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" TIMEOUT 240)
# ... that project packaged with the base hand as its VR hands, played in the simulated VR with the grab script: the
# Skeletal Mesh hands made (their bones found, the Open/Fist animations), shown and posed while the script grabs and throws
add_test(NAME vr_hand_package COMMAND realengine --project ${CMAKE_BINARY_DIR}/test_projects/VRHand/VRHand.rproject --open ${CMAKE_BINARY_DIR}/test_projects/VRHand/Content/VRRoom.rlevel --package ${CMAKE_BINARY_DIR}/test_game_vrhand --bench 1)
set_tests_properties(vr_hand_package PROPERTIES FIXTURES_REQUIRED vr_hand_saved FIXTURES_SETUP vr_hand_pkg)
add_test(NAME vr_hand_played COMMAND ${CMAKE_BINARY_DIR}/test_game_vrhand/VRRoom/VRRoom.exe --validation --windowed --vr-sim --vr-script --bench 5) # (no --shot: its long frame fell on the throw)
set_tests_properties(vr_hand_played PROPERTIES FIXTURES_REQUIRED vr_hand_pkg ENVIRONMENT "REALENGINE_XR_SIM_ONLY=1"
                     PASS_REGULAR_EXPRESSION "right hand /Content/VR/Hands/SK_BaseHand_R.rskm: 26 bones, fingers found, its Open/Fist animations.*VR script: thrown .* PASS" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(vr_selftest PROPERTIES PASS_REGULAR_EXPRESSION "VR self-test: 0 failures" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(mobile_selftest PROPERTIES PASS_REGULAR_EXPRESSION "mobile self-test: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME touch_selftest COMMAND realengine --touch-test)
set_tests_properties(touch_selftest PROPERTIES PASS_REGULAR_EXPRESSION "touch self-test: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME vehicle_selftest COMMAND realengine --vehicle-test)
set_tests_properties(vehicle_selftest PROPERTIES PASS_REGULAR_EXPRESSION "vehicle self-test: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME vehicles_scene COMMAND realengine --validation --scene vehicles --play --bench 8 --min-fps 200)
set_tests_properties(vehicles_scene PROPERTIES PASS_REGULAR_EXPRESSION "Vehicles: 11 simulated" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME particles_scene COMMAND realengine --validation --scene particles --cam 0,-1100,260,90,8 --bench 3 --min-fps 200)
set_tests_properties(particles_scene PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" PASS_REGULAR_EXPRESSION "Particles: 11 emitters") # (B10: the Ribbon Trail with them)
add_test(NAME particles_5m COMMAND realengine --scene particles5m --cam 0,-9000,5000,90,-22 --bench 13 --min-fps 180)
set_tests_properties(particles_5m PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|DEVICE_LOST" PASS_REGULAR_EXPRESSION "drawn 4[0-9][0-9][0-9][0-9][0-9][0-9] sprites")
# the graph menus (graph_menu.h): recents and recommendations learned and kept; a Blueprint's Event Graph and a function
add_test(NAME graph_menu COMMAND realengine --graph-menu-test)
add_test(NAME bp_graph_menu COMMAND realengine --validation --bp-menu-test /Content/MenuTest/BP_MenuTest.rbp --bench 1)
set_tests_properties(bp_graph_menu PROPERTIES ENVIRONMENT "REALENGINE_GRAPH_MENU_FILE=${CMAKE_BINARY_DIR}/graph_menu_test.txt"
                     FAIL_REGULAR_EXPRESSION "FAIL|VUID" PASS_REGULAR_EXPRESSION "bp-menu test: ok")
add_test(NAME skeletal_gizmo_move COMMAND realengine --validation --sk-move-test --bench 1)
# The GI (notas-dev/gi.md): Build Lighting (preview) then the baked path alone, validated; the fallback without ray
# tracing; both above the same 350 fps.
add_test(NAME gi_bake COMMAND realengine --validation --bake preview --gi off --bench 2 --min-fps 200)
add_test(NAME gi_no_rt COMMAND realengine --no-rt --bench 2 --min-fps 200)
# the GI's rays by software (A1, sw_rt.c): the house without ray queries, validated, its GI dynamic (the cache's charts
# and the probes traced by the BVHs), 200 fps on Medium, no flicker
add_test(NAME gi_house_sw COMMAND realengine --validation --no-rt --scene house --scalability 1 --bench 3 --min-fps 200 --max-flicker 0.12)
set_tests_properties(gi_house_sw PROPERTIES PASS_REGULAR_EXPRESSION "ray tracing por software \\(A1\\)" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# the far probes (A2, gi.glsl probe_grid 1): the landscape reaches past the near grid, so they run; validated (--play: its self-test)
add_test(NAME far_probes COMMAND realengine --validation --scene landscape --play --scalability 3 --bench 3 --min-fps 100)
set_tests_properties(far_probes PROPERTIES PASS_REGULAR_EXPRESSION "Sondas distantes \\(A2\\): ligadas" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# Scale (notas-dev/escala.md): a million actors with every ray traced effect for 6 s, no crash (the NVIDIA driver went
# down in its background shader compiler on 596.36; fixed in 617.42) and above 150 fps (178 measured).
add_test(NAME scale_rt COMMAND realengine --stress-objects 1000000 --gi full --bench 6 --min-fps 150)
# A million lights (notas-dev/luz.md: the grid of local lights), every pixel reached by ~40: above 150 fps (~165 measured).
add_test(NAME lights_scale COMMAND realengine --stress-lights 1000000 --bench 4 --min-fps 150)
# The packaged game (Build > Package Game): the editor saves its default level and packages it, then the game runs it
# from the package above the lema's ideal 350 fps (full screen) and with the validation layer quiet.
set(RE_TEST_GAME ${CMAKE_BINARY_DIR}/test_game)
file(MAKE_DIRECTORY ${RE_TEST_GAME})
add_test(NAME game_package COMMAND realengine --save ${RE_TEST_GAME}/Level.rlevel --package ${RE_TEST_GAME}/pkg --bench 1)
set_tests_properties(game_package PROPERTIES FIXTURES_SETUP game_pkg)
# Package Game's identity (sugestão #37): a picture made into the executable's icon (256, 64, 48, 32 and 16 px) with the
# game's version, and the game still starts (the resources written by BeginUpdateResource)
add_test(NAME game_package_icon COMMAND realengine --save ${RE_TEST_GAME}/Icon.rlevel --package ${RE_TEST_GAME}/pkg_icon
         --package-icon ${CMAKE_CURRENT_SOURCE_DIR}/data/branding/logo.png --bench 1)
set_tests_properties(game_package_icon PROPERTIES PASS_REGULAR_EXPRESSION "icon of 5 images" FIXTURES_SETUP game_pkg_icon)
add_test(NAME game_icon_runs COMMAND ${RE_TEST_GAME}/pkg_icon/Icon/Icon.exe --bench 1)
set_tests_properties(game_icon_runs PROPERTIES FIXTURES_REQUIRED game_pkg_icon)
add_test(NAME game_fps COMMAND ${RE_TEST_GAME}/pkg/Level/Level.exe --validation --bench 3 --min-fps 200)
# (B15a) Shipping's single content file: packaged with Content.rpak (the loose Content gone), the game reads it all from there
add_test(NAME game_package_rpak COMMAND realengine --save ${RE_TEST_GAME}/Packed.rlevel --package ${RE_TEST_GAME}/pkg_rpak --bench 1)
set_tests_properties(game_package_rpak PROPERTIES ENVIRONMENT "REALENGINE_PACK=1" PASS_REGULAR_EXPRESSION "Package: Content.rpak" FIXTURES_SETUP game_pkg_rpak)
# (A9.4) HDR10 output asked for: on where the display has it, else the game says so and stays in SDR (no errors either way)
add_test(NAME game_hdr10 COMMAND ${RE_TEST_GAME}/pkg_rpak/Packed/Packed.exe --validation --bench 2)
set_tests_properties(game_hdr10 PROPERTIES FIXTURES_REQUIRED game_pkg_rpak ENVIRONMENT "REALENGINE_HDR=1"
                     PASS_REGULAR_EXPRESSION "HDR10 output: (on|unavailable)" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q16) everything editable in the game (notas-dev/em-jogo.md): the Details' properties and the console variables checked
# and timed in the editor; a level whose map is made while playing (the procedural Facility) with a Blueprint that sets a
# console variable and its light's Intensity by name, then asks Build Lighting (Async), packaged; the game bakes it behind
# its loading screen, or while it plays (the bake in 1 ms of GPU a frame: no frame over 25 ms, 200 fps after it)
add_test(NAME ingame_package_loading COMMAND realengine --validation --ingame-test loading --ingame-package ${RE_TEST_GAME}/ingame_loading --bench 1)
add_test(NAME ingame_package_progressive COMMAND realengine --validation --ingame-test progressive --ingame-package ${RE_TEST_GAME}/ingame_progressive --bench 1)
set_tests_properties(ingame_package_loading PROPERTIES PASS_REGULAR_EXPRESSION "In-game settings: PASS" FAIL_REGULAR_EXPRESSION "FAIL|VUID"
                     FIXTURES_SETUP game_ingame_loading RESOURCE_LOCK ingame_content)
set_tests_properties(ingame_package_progressive PROPERTIES PASS_REGULAR_EXPRESSION "In-game settings: PASS" FAIL_REGULAR_EXPRESSION "FAIL|VUID"
                     FIXTURES_SETUP game_ingame_progressive RESOURCE_LOCK ingame_content)
add_test(NAME game_bake_loading COMMAND ${RE_TEST_GAME}/ingame_loading/BakeLoading/BakeLoading.exe --validation --bench 2)
add_test(NAME game_bake_progressive COMMAND ${RE_TEST_GAME}/ingame_progressive/BakeProgressive/BakeProgressive.exe --validation --bench 2
         --max-bake-frame-ms 25 --min-fps 200)
set_tests_properties(game_bake_loading PROPERTIES FIXTURES_REQUIRED game_ingame_loading PASS_REGULAR_EXPRESSION "Runtime lighting: completed"
                     FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(game_bake_progressive PROPERTIES FIXTURES_REQUIRED game_ingame_progressive PASS_REGULAR_EXPRESSION "Runtime lighting: completed"
                     FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME game_rpak_runs COMMAND ${RE_TEST_GAME}/pkg_rpak/Packed/Packed.exe --validation --bench 3 --min-fps 200)
set_tests_properties(game_rpak_runs PROPERTIES FIXTURES_REQUIRED game_pkg_rpak PASS_REGULAR_EXPRESSION "Pacote: .*arquivos" FAIL_REGULAR_EXPRESSION "FAIL|VUID|aviso: .*pacote")
# VR without a headset (--vr-sim: both eyes side by side): each eye a whole frame, 200 eye frames a second = 100 stereo
add_test(NAME vr_sim COMMAND ${RE_TEST_GAME}/pkg/Level/Level.exe --validation --vr-sim --bench 3 --min-fps 200)
set_tests_properties(vr_sim PROPERTIES FIXTURES_REQUIRED game_pkg PASS_REGULAR_EXPRESSION "VR: simulated")
# (Q25) the Quest's preset and the deepest foveation (vrs_fov.comp with MSAA 4x), both eyes in one frame: validation quiet, >= 200 frames/s
add_test(NAME mr_sim COMMAND ${RE_TEST_GAME}/pkg/Level/Level.exe --validation --vr-sim --cvar vr.MixedReality=1 --bench 3 --min-fps 200) # (Q25b) the packaged game in mixed reality on the simulated headset: the stand-in room, its real depth, validation clean, >= 200 pairs/s
set_tests_properties(mr_sim PROPERTIES FIXTURES_REQUIRED game_pkg ENVIRONMENT "REALENGINE_XR_SIM_ONLY=1" PASS_REGULAR_EXPRESSION "MR measure: passthrough simulated, room 10 " FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME vr_quest_sim COMMAND ${RE_TEST_GAME}/pkg/Level/Level.exe --validation --vr-sim --vr-ini vr_quality=4 --vr-ini vr_foveation=3 --vr-ini vr_foveation_dynamic=0 --bench 3 --min-fps 200)
set_tests_properties(vr_quest_sim PROPERTIES FIXTURES_REQUIRED game_pkg PASS_REGULAR_EXPRESSION "VR measure: .*foveation 3, shared work on")
add_test(NAME ar_sim COMMAND ${RE_TEST_GAME}/pkg/Level/Level.exe --validation --ar-image ${CMAKE_CURRENT_SOURCE_DIR}/data/branding/splash.jpg --ar-sim --bench 3 --min-fps 200)
set_tests_properties(ar_sim PROPERTIES FIXTURES_REQUIRED game_pkg PASS_REGULAR_EXPRESSION "ARCore: simulated") # the camera's picture behind the level, the floor receiving its shadows (ar_arcore.c)
set_tests_properties(game_fps PROPERTIES FIXTURES_REQUIRED game_pkg)
# The Quality mode (sugestão #45): the same packaged game drawn by the path tracer (denoised, reprojected), above 60 fps
add_test(NAME game_path_tracing COMMAND ${RE_TEST_GAME}/pkg/Level/Level.exe --validation --path-tracing --bench 3 --min-fps 60)
set_tests_properties(game_path_tracing PROPERTIES FIXTURES_REQUIRED game_pkg FAIL_REGULAR_EXPRESSION "FAIL")
# The first run of a game just packaged (an executable the driver never saw): first frame in under 1.5 s thanks to the
# pipeline cache the editor filled (cold: 4.6 s), and 350 fps from the start (tools/test_first_run.cmake)
add_test(NAME game_first_run COMMAND ${CMAKE_COMMAND} -DEDITOR=$<TARGET_FILE:realengine> -DOUT=${RE_TEST_GAME}/first_run
         -DMAX_START_MS=1500 -DMIN_FPS=250 -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_first_run.cmake)
# The Blueprints in the packaged game: the blueprints test packages its level, the game plays it (BeginPlay prints)
set_tests_properties(blueprints PROPERTIES FIXTURES_SETUP game_bp)
# In the game the floor must not flicker with the player standing (a body that never rests moved the camera)
add_test(NAME play_flicker COMMAND realengine --validation --bp-idle --bench 3 --min-fps 200 --max-flicker 0.1) # (300 fps before the post processing, notas-dev/render.md)
# ... nor with it walking through the house (a still camera: what changes is the light, not the view)
add_test(NAME house_walk COMMAND realengine --validation --scene house --bp-walkview --bench 3 --min-fps 200 --max-flicker 0.13) # (as gi_house)
add_test(NAME shadow_ghost COMMAND realengine --validation --scene shadow_ghost --play-eject --bench 3) # (2026-11-10) a cube sliding under three point lights at night: the many lights' shadow history (shadow_split.comp)
set_tests_properties(shadow_ghost PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# ... and the ray traced main pass's shader keeps everything in registers: a spill to local memory (96 registers and
# 112 bytes out, instead of 128 in) cost 10% of the pass on High; it moves with a few lines of mesh.frag or rt.glsl, the
# frame rate under a busy GPU doesn't show it, the driver's count does (notas-dev/render.md). Without that statistic: passes
add_test(NAME shader_spill COMMAND realengine --validation --no-spill --scene house --bench 1)
# the Debug Viewer open while playing: its lists filled, no validation error, its cost inside the budget
# the materials: the grid, the examples baked (Tlender nodes), a dynamic instance; validation, the frame rate
add_test(NAME materials COMMAND realengine --validation --scene materials --bench 2 --min-fps 200)
# the Principled's extra lobes (M1b): the materials' scene's second row (car paint, brushed metal, skin, glass) makes the
# RE_EXT pipelines on its first frame and draws with them, validated, on Epic (ray traced) and Low
add_test(NAME material_lobes COMMAND realengine --validation --scene materials --scalability 3 --bench 2 --min-fps 150)
add_test(NAME material_lobes_low COMMAND realengine --validation --scene materials --scalability 0 --bench 2 --min-fps 200)
# the skin's screen space subsurface scattering (A9.3a, sss.comp): the checkerboard put back together, Burley's profile,
# the transmission through the thin plates (Epic: ray traced); Low: the pass without the rays. Validated
add_test(NAME skin_sss COMMAND realengine --validation --scene skin --scalability 3 --bench 2 --min-fps 200)
add_test(NAME skin_sss_low COMMAND realengine --validation --scene skin --scalability 0 --bench 2 --min-fps 200)
# the eyes (A9.3c, the Eye shading model: M_Eye on SM_Eye, the iris refracted under the cornea) on the head, close up
add_test(NAME eyes COMMAND realengine --validation --scene eyes --scalability 3 --bench 2 --min-fps 200)
# (Q11) the Toon shading model and the Auto Adaptive Outline (notas-dev/toon.md, --scene toon): the examples drawn as
# Toon, the outlines drawn (their share of the view: the image's measure), their GPU time under 0.5 ms at 1440p
# (editor_toon_check), no flicker past the scene's own (the TSR keeps the lines still), validated. Epic (TSR, rays),
# Cinematic (MSAA 4x at full size), Low (shadow maps, no rays)
add_test(NAME toon COMMAND realengine --validation --scene toon --scalability 3 --bench 3 --min-fps 200 --max-flicker 0.6)
add_test(NAME toon_msaa COMMAND realengine --validation --scene toon --scalability 4 --bench 2 --min-fps 100)
add_test(NAME toon_low COMMAND realengine --validation --scene toon --scalability 0 --bench 2 --min-fps 200)
set_tests_properties(toon toon_msaa toon_low PROPERTIES PASS_REGULAR_EXPRESSION "Toon: outline [0-9]" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# the hair in strands (A9.3b, groom.c: a Groom on the head, its depth prepass then shaded, the main pass's targets
# written zero): Epic (4 targets with the denoiser's) and Low (1)
add_test(NAME hair COMMAND realengine --validation --scene hair --scalability 3 --bench 2 --min-fps 150)
add_test(NAME hair_low COMMAND realengine --validation --scene hair --scalability 0 --bench 2 --min-fps 150)
# (2026-11-16) the hair simulated (hair_sim.comp): the head swaying and turning in a wind, the strands swinging and colliding
# (2026-11-16, crashes #135-137) skins appearing and going every frame while the frame's bends are recorded: the GPU
# hung in "skin (ossos e BLAS)" (a job refit another skin's BLAS); 6 s of it must run clean
add_test(NAME skin_churn COMMAND realengine --validation --scene skin_churn --bench 6)
set_tests_properties(skin_churn PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST|stopped responding" PASS_REGULAR_EXPRESSION "Measure: ")
add_test(NAME hair_dynamic COMMAND realengine --validation --scene hair_dynamic --scalability 3 --bench 3 --min-fps 150)
# a river (B12c, river.c: a Water Body River's spline, its ribbon), three crates with Buoyancy floating down it and a
# Character swimming in it (B12f), validated in Play (editor_river_check); the camera under the water (B12h)
add_test(NAME river COMMAND realengine --validation --scene river --play --scalability 3 --bench 4 --min-fps 200)
set_tests_properties(river PROPERTIES PASS_REGULAR_EXPRESSION "River: 3 of 3 crates floating" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME underwater COMMAND realengine --validation --scene underwater --scalability 3 --bench 2 --min-fps 200)
set_tests_properties(underwater PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# Local Fog Volumes (A9.5b, weather.comp pass 3): a sphere and a box of fog lit in the froxels, no height fog
add_test(NAME fog_volume COMMAND realengine --validation --scene fogvol --scalability 3 --bench 2 --min-fps 200)
set_tests_properties(fog_volume PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# Heterogeneous Volumes (A9.5a, volume.c with the Tlender's OpenVDB reader): the example puff (24 .vdb frames written by
# vdb.c's writer, read back) played and drawn as the fires are
add_test(NAME volume_vdb COMMAND realengine --validation --scene volume --scalability 3 --bench 2 --min-fps 200)
set_tests_properties(volume_vdb PROPERTIES PASS_REGULAR_EXPRESSION "Volume: 24 frame" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST|aviso: Volume")
# de-lighting (A9.5e, delight.c): a sphere's checker lit from one side, its baked light taken out with its normals
add_test(NAME delight COMMAND realengine --delight-test)
set_tests_properties(delight PROPERTIES PASS_REGULAR_EXPRESSION "De-light: the light's spread" FAIL_REGULAR_EXPRESSION "FAIL")
# Gaussian Splatting (A9.5d, splat.c): the examples (a .splat bush, a 3DGS .ply flower bed) culled, sorted on the GPU
# and drawn over the scene
add_test(NAME splat COMMAND realengine --validation --scene splat --scalability 3 --bench 2 --min-fps 200)
set_tests_properties(splat PROPERTIES PASS_REGULAR_EXPRESSION "Splat: 40000 gaussians" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST|aviso: Splat")
# fire (A14, fire.c): a campfire's gas on its grid (7 passes and the pressure), drawn over the scene, its light flickering
add_test(NAME fire COMMAND realengine --validation --scene fire --scalability 3 --bench 3 --min-fps 150)
set_tests_properties(fire PROPERTIES PASS_REGULAR_EXPRESSION "Fire: 1 fire" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
set_tests_properties(hair hair_low PROPERTIES PASS_REGULAR_EXPRESSION "Groom: [0-9]+ strands" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
set_tests_properties(skin_sss skin_sss_low eyes PROPERTIES PASS_REGULAR_EXPRESSION "Subsurface \\(pele\\): [0-9]"
                     FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# the decals (M6, decal.c): grime on the floor and a wall, a small one with a life span; validated on Epic and Low
add_test(NAME decals COMMAND realengine --validation --scene decals --scalability 3 --bench 2 --min-fps 200)
add_test(NAME decals_low COMMAND realengine --validation --scene decals --scalability 0 --bench 2 --min-fps 300)
set_tests_properties(decals decals_low PROPERTIES PASS_REGULAR_EXPRESSION "Decals: 6 of 6 drawn" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# the unified ray traced denoiser (A9.1d, denoise.comp; the Photoreal style): the diffuse GI's and the rough reflections'
# rays and the shadows through one filter, validated; its flicker (alone 0.12-0.14%, area lights 0.10%) and its cost
# (house at Epic ~140 fps on a 3060: the Photoreal goal is 60)
add_test(NAME denoiser_house COMMAND realengine --validation --scene house --render-style 2 --scalability 3 --bench 3 --min-fps 90 --max-flicker 0.2)
add_test(NAME denoiser_area COMMAND realengine --validation --scene house --source-radius 30 --render-style 2 --scalability 3 --bench 3 --min-fps 90 --max-flicker 0.2)
set_tests_properties(denoiser_house denoiser_area PROPERTIES PASS_REGULAR_EXPRESSION "Denoiser: [0-9]" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q20) the GI and reflections stable (notas-dev/gi-estavel.md). The ghosting probe (exposure.comp pass 3: what still
# changes where a moving actor just left) behind the pillar sweeping across house_ghost; the flicker back and forth with
# the camera moving slowly (--cam-move). Limits over the measures (gi-estavel.md), never on them
add_test(NAME ghost_realistic COMMAND realengine --validation --scene house_ghost --scalability 3 --bench 3 --min-fps 150 --max-ghost 0.8)
add_test(NAME ghost_medium COMMAND realengine --validation --scene house_ghost --scalability 1 --bench 3 --min-fps 250 --max-ghost 0.7)
add_test(NAME ghost_photoreal COMMAND realengine --validation --scene house_ghost --render-style 2 --scalability 3 --bench 3 --min-fps 80 --max-ghost 1.9)
add_test(NAME stable_pan COMMAND realengine --validation --scene house --scalability 3 --cam-move -25,-12,0 --bench 3 --min-fps 200 --max-pulses 0.25)
add_test(NAME stable_pan_photoreal COMMAND realengine --validation --scene house --render-style 2 --scalability 3 --cam-move -25,-12,0 --bench 3 --min-fps 90 --max-pulses 0.45)
set_tests_properties(ghost_realistic ghost_medium ghost_photoreal stable_pan stable_pan_photoreal PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# the static meshes' BLAS made of clusters (A8, rt_clusters.c; needs an RTX with VK_NV_cluster_acceleration_structure),
# validated, flicker as the classic ones; and the classic path forced (REALENGINE_NO_CLAS: AMD, Intel, Android)
add_test(NAME rt_clusters COMMAND realengine --validation --scene house --scalability 3 --bench 2 --min-fps 200 --max-flicker 0.12)
set_tests_properties(rt_clusters PROPERTIES PASS_REGULAR_EXPRESSION "BLAS de clusters \\(A8\\)" FAIL_REGULAR_EXPRESSION "FAIL|VUID|nao deu")
add_test(NAME rt_classic COMMAND realengine --validation --scene house --scalability 3 --bench 2 --min-fps 200 --max-flicker 0.12)
set_tests_properties(rt_classic PROPERTIES ENVIRONMENT "REALENGINE_NO_CLAS=1" PASS_REGULAR_EXPRESSION "BLAS: [0-9]+ meshes" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# The render styles (notas-dev/render.md "Estilos de render", A9.0): Stylized, Photorealistic (AgX), Video-realistic (AgX and
# the camera's lens and film) on the materials' scene, validated (Realistic is the composite as it was)
add_test(NAME render_style_stylized COMMAND realengine --validation --scene materials --render-style 0 --bench 2 --min-fps 200)
add_test(NAME render_style_photoreal COMMAND realengine --validation --scene materials --render-style 2 --bench 2 --min-fps 200)
add_test(NAME render_style_video COMMAND realengine --validation --scene materials --render-style 3 --bench 2 --min-fps 200)
# Variable rate shading (notas-dev/render.md "VRS", A9.7b, opt-in): the rate image made and bound, validated, the house walked at Epic
add_test(NAME vrs COMMAND realengine --validation --scene house --bp-walkview --scalability 3 --bench 3 --budget 10)
set_tests_properties(vrs PROPERTIES ENVIRONMENT "REALENGINE_VRS=1" FAIL_REGULAR_EXPRESSION "FAIL|VUID" PASS_REGULAR_EXPRESSION "Budget ")
set_tests_properties(render_style_stylized render_style_photoreal render_style_video PROPERTIES PASS_REGULAR_EXPRESSION "Render style: " FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(material_lobes material_lobes_low PROPERTIES PASS_REGULAR_EXPRESSION "RE_EXT\\) made" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME constraints COMMAND realengine --validation --constraint-test --bench 3 --min-fps 200) # Physics Constraints: ball, hinge with a limit, fixed, slide with a motor (constraint_test.c)
add_test(NAME navmesh COMMAND realengine --validation --nav-test --bench 10 --min-fps 200) # the NavMesh: stairs, a ramp, a gap jumped over a Nav Link, a corridor and a slot too narrow (nav_test.c)
set_tests_properties(navmesh PROPERTIES PASS_REGULAR_EXPRESSION "NavMesh OK" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME char_physics COMMAND realengine --validation --char-physics-test --bench 3 --min-fps 200) # the Character steps up, pushes a body with physics, rides a moving platform (char_test.c)
# Morph targets (notas-dev/animacao.md "Morph targets"): a 16 k vertex grid with two targets imported from a glTF the test
# writes, its weights set by Set Morph Target's function, its skin read back from the GPU (morph_test.c), the cost measured
# the Tlender's shape keys (tests/data/import/shapekeys.tlend: a quad strip, "Up" weighted by its vertex group, "Wide"
# relative to Up) into morph targets: a one-bone Skeletal Mesh
add_test(NAME import_tlend_shapekeys COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SKEYS --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/shapekeys.tlend --bench 1)
set_tests_properties(import_tlend_shapekeys PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "1 skeletal meshes, 0 animations, 1 actors, 2 morph targets")
# the Tlender's hair (tests/data/import/hair.tlend: 8 guides on "Head", dynamic) and an OBJ's polylines
# (hair_curves.obj): .rgroom guides and a Groom grown from them (notas-dev/render.md "Cabelo do Tlender")
add_test(NAME import_tlend_hair COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/THAIR --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/hair.tlend --bench 1)
set_tests_properties(import_tlend_hair PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "1 hair grooms[^
]*
(.*
)*.*Groom: 8 guides read")
add_test(NAME import_obj_hair COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/OHAIR --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/hair_curves.obj --bench 1)
set_tests_properties(import_obj_hair PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "Groom: 6 guides read")
# (Q19) the Tlender imported whole (notas-dev/importar-tlender.md): q19_full.tlend's modifier stacks applied (Mirror, Array,
# Solidify, Subdivision, Displace, Decimate, Edge Split; Cloth into the Cloth component; Bevel lost), its nodes the Engine
# lacks converted or baked on the GPU (Brick, Stones, Blackbody, RGB Curves, Light Path, Metallic BSDF, AO, Attribute; a
# volume lost), a generated image, a missing one, the drivers (a corrective shape key baked into the morph curve, a
# constraint's influence, the scene's lost), a light and a camera: the report's counts, the import inside its budget (ms)
add_test(NAME import_tlend_q19 COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Q19FULL --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/q19_full.tlend --import-budget 4000 --bench 1)
set_tests_properties(import_tlend_q19 PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "1 animations, 4 actors, 1 morph targets, 1 lights, 1 cameras; report: 9 same, 16 converted, 5 baked, 4 lost")
# (Q19) what the Tlender's export brings for the Engine (make_q19_baked.py): an object's evaluated mesh ("evalmeshes": the
# Boolean the Engine can't apply) and nodes baked on the Tlender's GPU ("bake": a procedural Lava, an Ambient Occlusion)
add_test(NAME import_tlend_q19_baked COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Q19BAKED --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/q19_baked.tlend --import-budget 3000 --bench 1)
set_tests_properties(import_tlend_q19_baked PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "1 meshes, 1 materials \\(2 textures, 0 nodes skipped\\), 0 skeletal meshes, 0 animations, 1 actors; report: 4 same, 0 converted, 4 baked, 0 lost")
# (2026-11-19) a file of the Tlender's own Export > Real Engine (.tlend) (tlender_tests tlend-engine): its Soil material baked
# into color/roughness/normal (a Principled made of them, the normal through a Normal Map), an AO baked, a Bevel and a
# creased Subdivision in its evaluated meshes; the same file without them (as Ctrl+S saves it): the Subdivision with its
# crease applied here (OpenSubdiv's sharpness, as the Tlender's), the Bevel lost
add_test(NAME import_tlend_engine COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Q19TL --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/q19_tlender.tlend --import-budget 3000 --bench 1)
set_tests_properties(import_tlend_engine PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "4 meshes, 2 materials \(4 textures, 0 nodes skipped\), 0 skeletal meshes, 0 animations, 4 actors; report: 7 same, 2 converted, 4 baked, 0 lost")
add_test(NAME import_tlend_creases COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Q19CR --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/q19_tlender_ctrls.tlend --import-budget 3000 --bench 1)
set_tests_properties(import_tlend_creases PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "report: 8 same, 4 converted, 2 baked, 1 lost")
# (#164) a rigid rig (a robot, a capsule): a mesh parented to a bone with no Armature modifier (bone_parent.tlend: "Piece"
# on "Arm", the action "Spin" turning it) is a Skeletal Mesh part that rides that bone; the bone's custom shape ("Widget",
# bsobj) and a mesh in a collection off in render ("Hidden") are left out
add_test(NAME import_tlend_bone_parent COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/BONEPAR --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/bone_parent.tlend --bench 1)
set_tests_properties(import_tlend_bone_parent PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "1 meshes, 0 materials \(0 textures, 0 nodes skipped\), 1 skeletal meshes, 1 animations, 1 actors; report: 1 same, 0 converted, 0 baked, 0 lost")
# (2026-11-23) a piece on a bone with Relative Parenting (the Auto-Rig Pro's c_traj: boneinh): it follows the bone's
# deformation, not its tail (bone_relative.tlend = bone_parent.tlend with "boneinh 0 0 0 1 0" on Arm: at rest the Piece
# stays at its own 30 cm, not 200 cm up at the tail; the Capsula came 49 cm off its .rasset)
add_test(NAME import_tlend_bone_relative COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/BONEREL --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/bone_relative.tlend --bench 1)
set_tests_properties(import_tlend_bone_relative PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "SK_Rig: 2 bones, 1 skins, rest bounds [(]-25.0, -25.0, 30.0[)] - [(]25.0, 25.0, 30.0[)] cm")
# (#164) the exchange format (.rasset, the Tlender's Export > Real Engine; tests/data/import/make_rasset.py writes skin.rasset):
# a skinned strip welded back and bent by a 2-bone rig with its sampled animation and a morph target, a cube with hard
# edges, a material with the PNG the file carries, a light and a camera
add_test(NAME import_rasset COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/RASSET --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/skin.rasset --bench 1)
set_tests_properties(import_rasset PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID|damaged"
                     PASS_REGULAR_EXPRESSION "2 meshes, 1 materials \(1 textures, 0 nodes skipped\), 1 skeletal meshes, 1 animations, 2 actors, 1 morph targets, 1 lights, 1 cameras; report: 4 same, 2 converted, 0 baked, 0 lost")
add_test(NAME morph_targets COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/MORPH --template 0 --morph-test --bench 2 --min-fps 200)
set_tests_properties(morph_targets PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Morph test: OK")
# Inventory and Items (notas-dev/inventario.md): Item Definitions made in a new project, stacks that overflow, the weight
# limit, remove, use (a consumable), equip on the Mannequin's hand and unequip, drop and pick back up, Save / Load Game
# to Slot, the nodes and events through a Blueprint, the cost (inventory_test.c)
add_test(NAME inventory COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/INV --template 0 --inventory-test --bench 3 --min-fps 200)
set_tests_properties(inventory PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Inventory test: OK")
# Interaction (notas-dev/interacao.md): the self-test (focus by view, reach and priority, the conditions, once, hold, the
# door's swing and slide, the lever's switch, the chest's lid, 200 Interactables timed), then played in a new project: the
# player looks and the focus moves, the prompt on the screen, a door opened, a locked door (Item.Key.Red) refused and then
# opened with the key, a 1 s hold, a lever opening a gate, a chest giving its item (interaction_test.c)
add_test(NAME interaction COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/IA --template 0 --interaction-test --bench 4 --min-fps 200)
set_tests_properties(interaction PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Interaction test: OK")
# Dialogue (notas-dev/dialogo.md): a Dialogue made in a new project, its rules in the preview, then played with an NPC's
# Dialogue Speaker through Start Dialogue: a choice, a condition that hides an option, an action that gives an item and
# adds a tag, a jump, the end with its event, the language switched, WBP_Dialogue clicked, Save / Load Game to Slot, the
# cost (dialogue_test.c)
add_test(NAME dialogue COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DLG --template 0 --dialogue-test --bench 3 --min-fps 200)
set_tests_properties(dialogue PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Dialogue test: OK")
# Quests (notas-dev/missoes.md): the rules on made-up quests (in order, parallel, optional, a condition, the time limit,
# the save record, 200 quests timed), then a quest line played in a new project: Go To a Trigger Box, a Door used, herbs
# picked into the Inventory, wolves killed by Apply Damage (by their tag), a talk with an NPC's Dialogue, a Manual
# objective by the node; the rewards and the next quest, the events in a Blueprint, WBP_QuestTracker's texts, the marker
# at the edge with the distance, Save Progress / Load Progress and Save Game to Slot, the cost (quest_test.c)
add_test(NAME quest COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/QST --template 0 --quest-test --bench 3 --min-fps 200)
set_tests_properties(quest PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Quest test: OK"
                     ENVIRONMENT "REALENGINE_SAVE_DIR=${CMAKE_BINARY_DIR}/quest_test/SaveGames")
# Game Rules (notas-dev/regras-de-jogo.md): the self-test (the phases and their times, win by points, lose by time, kill
# points by tag, lives and respawn, elimination, rounds, the save chunk, the network message, 1000 deaths timed), then played
# in a new project: a BP_GameMode with the Game Rules, the countdown on WBP_MatchHUD, enemies killed for points, the player
# killed and respawned at the Player Start with its lives, the score target won (WBP_MatchEnd), Play Again, then a match lost
# by time, the events counted by a Blueprint, the cost (rules_test.c)
add_test(NAME rules COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/RULES --template 0 --rules-test --bench 10 --min-fps 200)
set_tests_properties(rules PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Game Rules test: OK")
# Abilities and effects (notas-dev/habilidades.md): the rules without the VM and 1000 actors with 5 effects each timed,
# then played in a new project: a Fireball (Mana cost, cooldown, its graph's Event On Activate Ability and End Ability), a
# stun that blocks it by its tag, a poison every second through the Health, stacks, a haste on the Character's speed,
# WBP_AbilityBar clicked, the events, Save / Load Progress with the effects and cooldowns (ability_test.c)
add_test(NAME ability COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/ABIL --template 0 --ability-test --bench 4 --min-fps 200)
set_tests_properties(ability PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_SAVE_DIR=${CMAKE_BINARY_DIR}/ability_test/SaveGames" FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Ability test: OK")
# HUD modular (notas-dev/hud.md): the self-test (markers, the feed's queue and stacking, the bar's trail, the bearing, the
# projection, the damage's direction, the hiding), then played in a new project: WBP_GameHUD from a HUD Layout, the nodes
# through a Blueprint, the health bar's loss and trail, a Blueprint variable's bar, Bind Attribute Bar, the crosshair, Set
# HUD Visible and the photo mode, the safe area and scale; 100 markers and 50 damage numbers timed (CPU and GPU) (hud_test.c)
add_test(NAME hud COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/HUD --template 0 --hud-test --bench 8 --min-fps 200)
# Sublevels (Window > Levels): made, actors placed into them, saved, opened, hidden, locked, moved; stackable Lighting Levels:
# Build Lighting bakes the persistent level and each Lighting Level alone, the sum loaded and timed, one switched off and on
# by Set Lighting Level Visible (levels_test.c, notas-dev/niveis.md)
add_test(NAME levels COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Levels --template 0 --levels-test --bench 12)
set_tests_properties(levels PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "levels-test: all passed")
# (2026-11-17, Q3) the user's case: a persistent level with no light, Lighting Levels Day and Night made with sky presets,
# baked each alone, switched on and off and blended (lights, sun, fog, clouds follow), saved and opened: no black level
add_test(NAME levels_lighting_sum COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/LevelsSum --template 0 --levels-test --bench 16)
set_tests_properties(levels_lighting_sum PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_LEVELS_TEST=sum" FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "levels-test: all passed")
# Removing a sublevel (its actors out, the others' marks renumbered, its file kept), undone and redone; the Outliner by
# level; a sublevel dragged to another place in the list; saved and opened in that order
add_test(NAME levels_remove COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/LevelsRemove --template 0 --levels-test --bench 3)
set_tests_properties(levels_remove PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_LEVELS_TEST=remove" FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "levels-test: all passed")
# (2026-11-19) the Levels panel itself in the editor: its controls clicked by injected mouse events and keys - the eye, the
# lock, At start, double-click (current), Lighting / Data, the Outliner by level, Del + Enter (removed), Ctrl+Z, trash + Esc
add_test(NAME levels_panel COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/LevelsPanel --template 0 --levels-test --bench 20)
set_tests_properties(levels_panel PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_LEVELS_TEST=panel" FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "levels-test: all passed")
# (2026-11-17, Q6) the asset picker (1000 textures: opened, searched, the three views and sizes, one chosen, the view kept in
# editor.ini) and Create from the selection (an IK Rig, a Material Instance, a Material and a Light Function; Add > IK Rig...
# through the picker): picker_test.c, notas-dev/content-browser.md
add_test(NAME asset_picker COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Picker --template 0 --picker-test)
set_tests_properties(asset_picker PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "picker-test: all passed" TIMEOUT 240)
set_tests_properties(hud PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "HUD test: OK")
# Pause and options (notas-dev/pausa-opcoes.md): the pause menu freezes the world (physics, a Blueprint's Tick) and not the
# UI nor a Tick When Paused actor; the Options by the keyboard; the Low preset applied; Jump remapped to F; saved, then read
# back by the next run (a new process)
add_test(NAME pause_options COMMAND realengine --validation --pause-test --bench 3 --min-fps 200)
set_tests_properties(pause_options PROPERTIES ENVIRONMENT "REALENGINE_SETTINGS_FILE=${CMAKE_BINARY_DIR}/pause_test/GameUserSettings.ini"
                     PASS_REGULAR_EXPRESSION "Pause test: OK" FAIL_REGULAR_EXPRESSION "FAIL" FIXTURES_SETUP pause_saved)
add_test(NAME pause_options_reload COMMAND realengine --validation --pause-test-reload --bench 1)
set_tests_properties(pause_options_reload PROPERTIES ENVIRONMENT "REALENGINE_SETTINGS_FILE=${CMAKE_BINARY_DIR}/pause_test/GameUserSettings.ini"
                     PASS_REGULAR_EXPRESSION "Pause reload: OK" FAIL_REGULAR_EXPRESSION "FAIL" FIXTURES_REQUIRED pause_saved)
# Save and load the progress (notas-dev/salvar-progresso.md): the format's self-test (checksum, a damaged slot refused,
# a version 1 save migrated), then played in a new project: saved, the world changed, loaded (Open Level) and checked
# (place, Health, Inventory, a destroyed actor, a SaveGame variable, a spawned actor, the Save Game object), a Checkpoint
# and the respawn, an auto save, an old save, WBP_SaveSlots; 1000 Saveables saved and loaded, timed (save_test.c)
add_test(NAME save_progress COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SAVE --template 0 --save-test --bench 3 --min-fps 200)
set_tests_properties(save_progress PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_SAVE_DIR=${CMAKE_BINARY_DIR}/save_test/SaveGames"
                     FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID" PASS_REGULAR_EXPRESSION "Save test: OK")
add_test(NAME gameplay_tags COMMAND realengine --tags-test) # Gameplay Tags: hierarchy, exact, add / remove (gameplay_tags.c)
# Health and damage (health.c, health_test.c): the self-test (types, shield, regeneration, death, heal, revive, radial,
# 1000 actors a frame timed), then played: a Blueprint's Event AnyDamage / On Death / On Healed / On Shield Broken, the
# radial damage node, the destroy, WBP_HealthBar on the screen
add_test(NAME health COMMAND realengine --validation --health-test --bench 3 --min-fps 200)
set_tests_properties(health PROPERTIES PASS_REGULAR_EXPRESSION "Health test: OK" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME mat_pins COMMAND realengine --mat-pins-test) # the Principled/Glossy inputs greyed as without effect are exactly those the compiler does not read (notas-dev/materiais.md)
# physics: a pyramid, a tower, balls, a box that breaks: everything rests on the floor (and the box broke)
add_test(NAME physics COMMAND realengine --validation --scene physics --play --bench 6 --min-fps 200)
# 1000 bodies falling into a heap: it comes to rest; the step's peak is printed (notas-dev/fisica.md)
add_test(NAME physics_stress COMMAND realengine --scene physics_stress --play --bench 12)
# ... and with cylinders and cones among them (GJK and EPA, their manifolds): it rests too
add_test(NAME physics_stress_shapes COMMAND realengine --scene physics_stress_shapes --play --bench 12)
# the collision shapes (notas-dev/fisica.md "Formas de colisão"): cylinders and cones standing, fallen over and rolled, a
# capsule, three convex hulls of an imported prism stacked, boxes and balls on an imported staircase by its triangles
add_test(NAME physics_shapes COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Shapes --template 0
         --scene physics_shapes --play --bench 10 --min-fps 200)
set_tests_properties(physics_shapes PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Shapes: 16 bodies checked" FAIL_REGULAR_EXPRESSION "FAIL")
# (Q8, phys_test.c / phys_joints_test.c; notas-dev/fisica.md "Corpos rígidos profissionais") a pyramid of 1035 boxes (45 at the base)
# stands and falls asleep with the default 4 substeps; awake (sleeping off) with 6 it never creeps; bullets up to 1000 m/s
# stop at a 2 cm wall; each joint (hinge, motor, slider, cone, linear and angular drives, soft limit, break) does what it
# says; 20 thousand bodies in an arena: the frame rate and the step's phases (its fps the measure, not yet the 200 asked)
add_test(NAME phys_stack COMMAND realengine --scene phys_stack --play --bench 6)
add_test(NAME phys_stack_awake COMMAND realengine --scene phys_stack_awake --play --bench 6)
add_test(NAME phys_ccd COMMAND realengine --validation --scene phys_ccd --play --bench 3 --min-fps 200)
add_test(NAME phys_joints COMMAND realengine --validation --scene phys_joints --play --bench 5 --min-fps 150)
add_test(NAME phys_heavy COMMAND realengine --scene phys_heavy --play --bench 10)
set_tests_properties(phys_stack phys_stack_awake PROPERTIES PASS_REGULAR_EXPRESSION "phys_stack: 1035 boxes" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(phys_ccd PROPERTIES PASS_REGULAR_EXPRESSION "phys_ccd: 8 bullets" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(phys_joints PROPERTIES PASS_REGULAR_EXPRESSION "phys_joints: [0-9]+ joints" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(phys_heavy PROPERTIES PASS_REGULAR_EXPRESSION "phys_heavy: 20000 bodies" FAIL_REGULAR_EXPRESSION "FAIL")
# (the Q8 leftover, notas-dev/fisica.md "20 mil corpos") 20 thousand crates and barrels asleep in a warehouse, an explosion every
# 1.5 s: the frame rate with 20 thousand Movable bodies, Physics Quality Medium (p.Rate 60); nothing through the floor
add_test(NAME phys_crates COMMAND realengine --scene phys_crates --play --bench 10 --cvar p.Rate=60 --min-fps 100)
set_tests_properties(phys_crates PROPERTIES PASS_REGULAR_EXPRESSION "phys_crates: 20000 bodies" FAIL_REGULAR_EXPRESSION "FAIL")
# animation: 1000 Mannequins walking, running, idling (notas-dev/animacao.md); the frame rate stays above 80 (the GPU shades the crowd)
# AI: a Behavior Tree hunter and two Brain guards in the house: they see, chase and meet (notas-dev/ia.md)
add_test(NAME ai COMMAND realengine --validation --scene ai --play --bench 6 --min-fps 200)
# a squad (notas-dev/ia.md): told by the one that sees, aiming where the runner will be, a ring closed round it
# Landscape (notas-dev/landscape.md): its chunks and plants, a Character dropped on it standing, the navigation over the hills
add_test(NAME landscape COMMAND realengine --validation --scene landscape --play --bench 4 --min-fps 180 --cache-all)
# (B11) the Landscape Layers: three materials painted in circles (the layered material in mesh.frag), validated
add_test(NAME landscape_paint COMMAND realengine --validation --scene landscape_paint --bench 4 --min-fps 180)
set_tests_properties(landscape_paint PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# Import from File (a .r16 ramp: the heights within 1 cm) and the Foliage brush (painted up to the density inside it, erased)
add_test(NAME landscape_heightmap COMMAND realengine --validation --scene landscape_hm --bench 2 --min-fps 200)
set_tests_properties(landscape PROPERTIES PASS_REGULAR_EXPRESSION "Landscape: 1 landscapes" FAIL_REGULAR_EXPRESSION "FAIL")
# the animation packs (notas-dev/animacao.md): the male / female Mannequins, the Locomotion pack, the creatures, all playing
add_test(NAME anim_packs COMMAND realengine --validation --scene packs --play --bench 2 --min-fps 200)
set_tests_properties(anim_packs PROPERTIES PASS_REGULAR_EXPRESSION "Packs: [0-9]+ files" FAIL_REGULAR_EXPRESSION "FAIL")
# Auto Rig (notas-dev/animacao.md): a T-posed statue rigged onto the Mannequin's skeleton, walking
add_test(NAME autorig COMMAND realengine --validation --scene autorig --play --bench 2 --min-fps 200)
set_tests_properties(autorig PROPERTIES PASS_REGULAR_EXPRESSION "Auto Rig: SK_" FAIL_REGULAR_EXPRESSION "FAIL")
# Viewport 2 (notas-dev/render.md): the second 3D view, ray traced from its own camera next to the main one
add_test(NAME view2 COMMAND realengine --validation --scene proto --play --view2 top --bench 2 --min-fps 200)
add_test(NAME view2_pt COMMAND realengine --validation --scene proto --play --view2 persp:pt --bench 2 --min-fps 200)
set_tests_properties(view2 view2_pt PROPERTIES PASS_REGULAR_EXPRESSION "Viewport 2: [0-9]+x[0-9]+, mode [0-9], [1-9][0-9]* frames" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# playable path tracing (notas-dev/render.md): a blockout with an animated character, history reprojected and denoised
add_test(NAME path_trace_play COMMAND realengine --validation --scene proto --play --view pt --bench 3 --min-fps 150)
set_tests_properties(path_trace_play PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# prototyping (notas-dev/prototipagem.md): a blockout's boxes collide (a Character on the platform) and the navigation climbs its stairs
add_test(NAME proto COMMAND realengine --validation --scene proto --play --bench 3 --min-fps 200)
set_tests_properties(proto PROPERTIES PASS_REGULAR_EXPRESSION "Prototyping: [0-9]+ boxes" FAIL_REGULAR_EXPRESSION "FAIL")
# UMG (notas-dev/umg.md): a Widget Blueprint shown by an actor, its bindings, its Button's click
add_test(NAME umg COMMAND realengine --validation --umg-test --bench 2 --min-fps 200)
set_tests_properties(umg PROPERTIES PASS_REGULAR_EXPRESSION "UMG: 1 widget" FAIL_REGULAR_EXPRESSION "FAIL")
# Sequencer (notas-dev/sequencer.md): a Level Sequence (Transform keys, Camera Cuts, an Event) made, saved, read back and played
# by a Blueprint's Play Sequence in PIE: the cube at its last key, the Custom Event answered, the cut's view, > 250 fps
add_test(NAME sequencer COMMAND realengine --validation --seq-test --bench 3 --min-fps 200)
# the editor keeps drawing while an import, the meshes read again, the materials baked and a cancelled task run in the background (notas-dev/async.md)
add_test(NAME async COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Async --template 0
         --async-test ${CMAKE_SOURCE_DIR}/tests/data/import/test.glb --bench 2)
set_tests_properties(async PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Async OK" FAIL_REGULAR_EXPRESSION "FAIL")
# the Assistant (notas-dev/assistente.md): its base packed from assistant/ (as published), read from that folder like
# the online fetch, kept offline in a folder of its own; the questions' answers in Portuguese and English, typos, errors
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME assistant_pack COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/assistant_pack.py ${CMAKE_BINARY_DIR}/assistant)
    set_tests_properties(assistant_pack PROPERTIES FIXTURES_SETUP assistant_base)
    add_test(NAME assistant COMMAND realengine --validation --assistant-test --bench 2)
    set_tests_properties(assistant PROPERTIES FIXTURES_REQUIRED assistant_base PASS_REGULAR_EXPRESSION "Assistant OK" FAIL_REGULAR_EXPRESSION "FAIL"
                         ENVIRONMENT "REALENGINE_ASSISTANT_URL=${CMAKE_BINARY_DIR}/assistant;REALENGINE_ASSISTANT_DIR=${CMAKE_BINARY_DIR}/assistant_offline")
    # no color literal outside theme.c (Q1); the themes' contrast by the script (WCAG 2)
    add_test(NAME theme_literals COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/theme_literals.py ${CMAKE_SOURCE_DIR}/src)
    set_tests_properties(theme_literals PROPERTIES PASS_REGULAR_EXPRESSION "theme_literals: OK")
    # (Q5) no control without a description: every literal label of src/ found in ui_tips_table*.c
    add_test(NAME tips_audit COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tips_audit.py ${CMAKE_SOURCE_DIR}/src)
    # (Q16) what is still editor only (tools/ingame_sweep.py): src/props_table.inc fresh, nothing new without a path in the game
    add_test(NAME ingame_sweep COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ingame_sweep.py --check)
    set_tests_properties(ingame_sweep PROPERTIES PASS_REGULAR_EXPRESSION "ingame_sweep: OK")
    set_tests_properties(tips_audit PROPERTIES PASS_REGULAR_EXPRESSION "tips_audit: OK")
    add_test(NAME theme_contrast COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/theme_contrast.py $<TARGET_FILE:re_test_theme>)
    set_tests_properties(theme_contrast PROPERTIES PASS_REGULAR_EXPRESSION "contrast: 11 theme\\(s\\), 0 failing" FAIL_REGULAR_EXPRESSION "FAIL")
endif()
add_test(NAME scripts COMMAND realengine --validation --script-test --bench 3 --min-fps 200) # the same class in Real, C#, Python and C (notas-dev/scripts.md)
set_tests_properties(scripts PROPERTIES PASS_REGULAR_EXPRESSION "Scripts OK" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME audio_play COMMAND realengine --validation --audio-test --bench 3 --min-fps 200) # sounds in PIE: Ambient Sound, component events, nodes, the stop (notas-dev/audio.md)
set_tests_properties(audio_play PROPERTIES PASS_REGULAR_EXPRESSION "Audio OK" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(sequencer PROPERTIES PASS_REGULAR_EXPRESSION "Sequencer OK" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME ai_squad COMMAND realengine --validation --scene ai_squad --play --bench 6 --min-fps 200)
# (Q24) the damage sense (a Victim hit from behind turns and sees the Shooter; the Control knows nothing) and the local
# avoidance (two Characters head-on pass each other and arrive) (ai_senses_test.c, notas-dev/ia.md)
add_test(NAME ai_senses COMMAND realengine --validation --scene ai_senses --play --bench 6 --min-fps 200)
set_tests_properties(ai_senses PROPERTIES PASS_REGULAR_EXPRESSION "ai_senses: PASS" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME ai_squad_mm COMMAND realengine --validation --scene ai_squad_mm --play --bench 6 --min-fps 200) # (Motion Matching: notas-dev/animacao.md)
set_tests_properties(ai_squad ai_squad_mm PROPERTIES PASS_REGULAR_EXPRESSION "AI squad: 4 of 4" FAIL_REGULAR_EXPRESSION "FAIL")
# Motion Matching's made starts and stops (notas-dev/animacao.md): idle, walk, stop, run, stop, jog, stop by a script; they play, the feet keep their place
add_test(NAME mm_startstop COMMAND realengine --validation --scene mm_startstop --play --bench 14 --min-fps 200)
# its examples pack: four characters of their own styles, only idle / walk / run (and strafes) given, the same script
add_test(NAME mm_examples COMMAND realengine --validation --scene mm_examples --play --bench 14 --min-fps 200)
set_tests_properties(mm_startstop mm_examples PROPERTIES PASS_REGULAR_EXPRESSION "Motion Matching starts and stops: " FAIL_REGULAR_EXPRESSION "FAIL")
# one long take as the whole database (the examples' LongTake): its moments found, its turns turning the actor
add_test(NAME mm_longtake COMMAND realengine --validation --scene mm_longtake --play --bench 14 --min-fps 200)
set_tests_properties(mm_longtake PROPERTIES PASS_REGULAR_EXPRESSION "Motion Matching long take: " FAIL_REGULAR_EXPRESSION "FAIL")
# the user's own starts and stops (the examples' Authored): played before the made ones
add_test(NAME mm_authored COMMAND realengine --validation --scene mm_authored --play --bench 14 --min-fps 200)
set_tests_properties(mm_authored PROPERTIES PASS_REGULAR_EXPRESSION "Motion Matching authored: " FAIL_REGULAR_EXPRESSION "FAIL")
# a take with jumps (the examples' JumpTake): Jump finds its takeoffs, its vertical root motion carries the capsule, it lands
add_test(NAME mm_jump COMMAND realengine --validation --scene mm_jump --play --bench 14 --min-fps 200)
set_tests_properties(mm_jump PROPERTIES PASS_REGULAR_EXPRESSION "Motion Matching jumps: " FAIL_REGULAR_EXPRESSION "FAIL")
# projects (notas-dev/projetos.md): new ones from the templates, played; their classes compile, the frame rate holds
add_test(NAME projects_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/test_projects)
set_tests_properties(projects_clean PROPERTIES FIXTURES_SETUP projects)
# Texture streaming (notas-dev/materiais.md): 400 cubes each with its own 4096 Material Instance (72 GB at full size)
# under a 1.5 GB budget, the camera flying along them: the budget kept, cubes climbing as it nears and going down as it
# leaves, above 250 fps (~610 measured). In a project of its own (its 401 assets would go into every other test's game).
add_test(NAME texture_streaming COMMAND realengine --new-project ${CMAKE_BINARY_DIR}/test_projects/TexStream --template 0 --stress-textures 400 --bench 8 --min-fps 200)
# The heavy model (notas-dev/render.md "Malhas pesadas"): 2.1 M triangles, 600 k of them Masked cards, 4096 materials,
# nine copies out to 60 m, in a project of its own (never the build's Content); welded, ordered, LODs (mesh_build.c),
# the packed draws. Geometry alone (no ray tracing) at Epic: 63 fps measured
add_test(NAME heavy_mesh COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Heavy --template 0 --scene heavy --scalability 3 --no-rt --bench 4 --min-fps 75)
set_tests_properties(heavy_mesh PROPERTIES ENVIRONMENT "REALENGINE_NO_SWRT=1") # (the raster of heavy meshes: without the GI's rays by software, A1, which its 2.1 M triangles of foliage hold near 50 fps: notas-dev/gi.md)
# The frame budget (notas-dev/orcamento.md; the plan's R11): Epic at 1440p in 10 ms. The house fits every slice (strict);
# Oakvale (the RPG, played) fits the whole frame, its sky and clouds still past their 1 ms slice; the heavy model is
# guarded at 18 ms until the upscalers bring its internal resolution down (its geometry alone ~11.8 ms of 15.7)
add_test(NAME budget_house COMMAND realengine --validation --scene house --scalability 3 --bench 4 --budget 10 --budget-strict)
add_test(NAME budget_rpg COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoRPGBudget --template 5 --play --scalability 3 --bench 5 --budget 10)
add_test(NAME budget_heavy COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/HeavyBudget --template 0 --scene heavy --scalability 3 --bench 4 --budget 18)
# Upscaling (notas-dev/upscaling.md): the modes, the fallback to TSR, the dynamic resolution converging (simulated); then the
# heavy model at Epic, Quality mode with the dynamic resolution at 100 fps: it fits the 10 ms (measured ~8.7 ms at 35%)
add_test(NAME upscale COMMAND realengine --upscale-test)
# the vendors' upscalers on this GPU (upscale_vendor.c; only when the build has their SDK: tools/fetch_upscalers.ps1):
# each one runs in TSR's place without a validation error, its flicker at rest under a limit (notas-dev/upscaling.md)
foreach(u dlss fsr xess)
    if(u STREQUAL "dlss")
        set(u_dll ${RE_UPS}/dlss/bin/nvngx_dlss.dll)
        set(u_name "NVIDIA DLSS")
        set(u_flicker 8) # (DLSS follows the auto exposure's ramp worse: 4-5% while it adapts, 0.9% once still)
    elseif(u STREQUAL "fsr")
        set(u_dll ${RE_UPS}/fsr/bin/amd_fidelityfx_vk.dll)
        set(u_name "AMD FSR")
        set(u_flicker 1.2)
    else()
        set(u_dll ${RE_UPS}/xess/bin/libxess.dll)
        set(u_name "Intel XeSS")
        set(u_flicker 1.5)
    endif()
    if(EXISTS ${u_dll})
        add_test(NAME upscaler_${u} COMMAND realengine --validation --scene house --bench 3 --upscaler ${u} --max-flicker ${u_flicker})
        set_tests_properties(upscaler_${u} PROPERTIES PASS_REGULAR_EXPRESSION "Upscaler: ${u_name} [0-9.]+ active" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
    endif()
endforeach()
set_tests_properties(upscale PROPERTIES PASS_REGULAR_EXPRESSION "Upscale test: OK" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME budget_heavy_dynres COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/HeavyDynRes --template 0 --scene heavy --scalability 3 --upscale-mode 2 --dyn-res 100 --dyn-res-min 35 --bench 8 --budget 10) # (the heavy stress scene needs 35%: the Auto floor is 50%)
set_tests_properties(budget_house budget_rpg budget_heavy budget_heavy_dynres PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|VUID" PASS_REGULAR_EXPRESSION "Budget ")
set_tests_properties(budget_rpg PROPERTIES ENVIRONMENT "REALENGINE_DEMO_AUTOPLAY=1")
# Translucent materials (notas-dev/render.md "Translúcidos"): weighted blended OIT, 3 big panes and 400 small ones, with validation
# The cluster LOD DAG (notas-dev/render.md "LOD de clusters"): 100 rocks of 1.54 M triangles, the camera low among them,
# each near one drawn by its DAG's cut (Epic 1 px), validated: ~167 fps (without the DAG ~147)
add_test(NAME cluster_lod COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Rocks --template 0 --scene rocks --scalability 3 --bench 3 --min-fps 120)
# (Q17) Nanite Tessellation (notas-dev/tessellation.md): the same 100 rocks with the noise as a real displacement, diced on the
# GPU (Epic 2 px a micro edge), validated, with its cost; one rock whole in the view, every edge's points hashed by both
# triangles along it (no crack: the hashes cancel); Low displaces the vertices alone; a tiny budget coarsens the dicing
add_test(NAME tess_rocks COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/RocksTess --template 0 --scene rocks_tess --scalability 3 --bench 4 --min-fps 110)
set_tests_properties(tess_rocks PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Geometry Tessellation: [0-9.]+ M micro triangles from [1-9]" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME tess_seams COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/TessSeams --template 0 --scene tess_seams --scalability 3 --bench 4)
set_tests_properties(tess_seams PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_NO_DAG_OCCLUSION=1" PASS_REGULAR_EXPRESSION "Tess seams: .*: OK" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME tess_low COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/RocksTessLow --template 0 --scene rocks_tess --scalability 0 --bench 3 --min-fps 120)
set_tests_properties(tess_low PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Geometry Tessellation: .*displaced only" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME tess_budget COMMAND realengine --new-project ${CMAKE_BINARY_DIR}/test_projects/RocksTessBudget --template 0 --scene rocks_tess --scalability 3 --bench 3)
set_tests_properties(tess_budget PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_TESS_BUDGET=0.1" PASS_REGULAR_EXPRESSION "Geometry Tessellation: .*scaled by the budget" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# (Q21) The distant view (notas-dev/visao-distante.md): a lookout over 8 km of mountains at a low sun (36 thousand trees of a
# Foliage actor, 3000 rocks in a World Partition with its HLOD, a height fog): the far shadow checked on the CPU's twin of the
# horizon pass, the tree's and the rock's impostors baked and drawn, the cost on Epic and on Low (hidden: Epic 144 fps, Low
# 247); the switch to the cards seen from a still camera (three shots: what the switch changes past the view's own drift);
# the flight over the forest (the cards turning and switching every frame: its cost)
add_test(NAME vista COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Vista --template 0 --scene vista --scalability 3 --bench 5 --min-fps 100)
set_tests_properties(vista PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Distant view: far shadow on .* 2 impostors" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME vista_low COMMAND realengine --new-project ${CMAKE_BINARY_DIR}/test_projects/VistaLow --template 0 --scene vista --scalability 0 --bench 4 --min-fps 170)
set_tests_properties(vista_low PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Distant view: far shadow on" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME vista_pop COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/VistaPop --template 0 --scene vista_pop --scalability 3 --bench 4)
set_tests_properties(vista_pop PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Vista pop: meshes -> cards" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME vista_walk COMMAND realengine --new-project ${CMAKE_BINARY_DIR}/test_projects/VistaWalk --template 0 --scene vista_walk --scalability 3 --bench 5 --cam-move 1400,1400,0 --min-fps 90)
set_tests_properties(vista_walk PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Vista walk: [0-9.]+% of the view" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
add_test(NAME translucent COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Glass --template 0 --scene glass --scalability 3 --bench 3 --min-fps 150)
set_tests_properties(translucent PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
# (2026-11-17, Q4) Transmission on Medium (no colored shadow without rays): the amber pane and the Thin curtain made and drawn
add_test(NAME transmission_medium COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/GlassMedium --template 0 --scene glass --scalability 1 --bench 3 --min-fps 150)
set_tests_properties(transmission_medium PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# (Q4) Light Functions: a projector's slide, blinds, a flicker and the clouds in the sun (the RE_EXT variants), validated
add_test(NAME light_function COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/LightFn --template 0 --scene lightfn --scalability 3 --bench 3 --min-fps 200)
set_tests_properties(light_function PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Light Function: the RE_EXT" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# (Q4) Interior Mapping: four buildings of rooms behind windows (day and night), validated
add_test(NAME interior_mapping COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/Interiors --template 0 --scene interiors --scalability 3 --bench 3 --min-fps 150)
set_tests_properties(interior_mapping PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# (Q4) the rain's drops as clear water: the scene's copy read behind them (Epic), validated
add_test(NAME rain_clear_drops COMMAND realengine --validation --scene rain --scalability 3 --bench 3 --min-fps 200)
set_tests_properties(rain_clear_drops PROPERTIES PASS_REGULAR_EXPRESSION "the drops read the scene behind them" FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# (2026-11-10) Masked / Two Sided cards with a coat and anisotropy (the masked runs' RE_EXT pipelines), Epic with ray
# tracing and Low, validated (notas-dev/render.md "Lóbulos extras em Masked"): 80 cards layered over the whole screen,
# its own goal (~215 fps measured on Epic, 1440p, RTX 3060)
add_test(NAME masked_lobes COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/MaskedLobes --template 0 --scene masked_lobes --scalability 3 --bench 3 --min-fps 170)
add_test(NAME masked_lobes_low COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/MaskedLobesLow --template 0 --scene masked_lobes --scalability 0 --no-rt --bench 3 --min-fps 200)
set_tests_properties(masked_lobes masked_lobes_low PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" PASS_REGULAR_EXPRESSION "masked runs' RE_EXT")
set_tests_properties(texture_streaming PROPERTIES ENVIRONMENT "REALENGINE_TEX_BUDGET_MB=512" FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL")
# (Q18) UDIM (notas-dev/udim.md): the .rudim format, names, codecs and versions (no GPU); an OBJ naming a tile imported to
# one .rudim; a 4 x 2 tile floor shot from above, unlit, read back: no step between neighbor pixels over 6 levels (a seam
# of the mips, a bleed, a missing tile); a 20-tile road under a 256 MB budget with the camera flying along it: only the
# tiles seen resident, the budget kept, climbs and descents
# (2026-11-23) udim_seams also bakes a material whose .rudim has one tile (1001 alone, as an imported 4K): its source read
# at the bake's size, not the 64² it is loaded at
# (2026-11-23) the Material Editor by injected mouse and keys (src/mat_editor_test.inc): a node dragged is drawn with the
# mouse in the same frame (it showed a frame behind); a link dropped on the empty canvas opens the add menu of what takes
# it and links the node picked, from an output (Mix Shader after the BSDF) and backwards from an input (Image Texture
# into the Base Color)
add_test(NAME mated_ui COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/MatEdUi --template 0 --bench 12)
set_tests_properties(mated_ui PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_MATED_TEST=1" FAIL_REGULAR_EXPRESSION "FAIL|VUID" PASS_REGULAR_EXPRESSION "mat-editor-test: all passed")
# (2026-11-23) an emissive material lights the scene (notas-dev/luz.md "Material emissivo ilumina"): a closed dark room lit
# only by a glowing sphere, its floor read back from the shot; Emissive Light 0: no light made
add_test(NAME emissive_light COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/EmitLight --template 0 --scene emit --bench 4 --shot ${CMAKE_BINARY_DIR}/emissive_light.png --min-fps 200)
add_test(NAME emissive_light_off COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/EmitLightOff --template 0 --scene emit_off --bench 2 --shot ${CMAKE_BINARY_DIR}/emissive_light_off.png)
set_tests_properties(emissive_light emissive_light_off PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" PASS_REGULAR_EXPRESSION "emit: ok")
# (2026-11-23) the viewport's gizmo by injected mouse and keys (src/gizmo_test.inc): the X arrow moves along X alone with
# one undo step, a click that never moves makes none, Y mid-drag moves the constraint, "250" typed is 250 cm, the right
# button puts it back, Rotate about the Median Point with "90" typed, Scale with "2" typed, a top view shows the gizmo
add_test(NAME gizmo_ui COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/GizmoUi --template 0 --bench 10)
set_tests_properties(gizmo_ui PROPERTIES FIXTURES_REQUIRED projects ENVIRONMENT "REALENGINE_GIZMO_TEST=1" FAIL_REGULAR_EXPRESSION "FAIL|VUID" PASS_REGULAR_EXPRESSION "gizmo-test: all passed")
add_test(NAME udim_format COMMAND realengine --udim-test)
add_test(NAME udim_import COMMAND realengine --udim-import-test)
add_test(NAME udim_seams COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/UdimSeams --template 0 --scene udim_seams --bench 3 --shot ${CMAKE_BINARY_DIR}/udim_seams.png --min-fps 200)
add_test(NAME udim_stream COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/UdimStream --template 0 --scene udim_stream --bench 8 --min-fps 200)
set_tests_properties(udim_format udim_import PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(udim_seams PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" PASS_REGULAR_EXPRESSION "UDIM seams: the largest step")
set_tests_properties(udim_stream PROPERTIES ENVIRONMENT "REALENGINE_TEX_BUDGET_MB=256" FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST" PASS_REGULAR_EXPRESSION "UDIM: [0-9]+ tiles")
add_test(NAME project_third_person COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/TP --template 1 --play --bench 3 --min-fps 200)
# (2026-11-10) the Third Person character's Blueprint Viewport shows its Mannequin: a Skeletal Mesh component bigger than the
# node's 96 bytes was dropped (no mesh in the level nor in the Viewport), and the Viewport drew only Static Meshes
add_test(NAME bp_viewport_character COMMAND realengine --validation --project ${CMAKE_BINARY_DIR}/test_projects/TP/TP.rproject
         --open-viewport /Content/ThirdPerson/BP_ThirdPersonCharacter.rbp --bench 2)
set_tests_properties(bp_viewport_character PROPERTIES FIXTURES_REQUIRED tp_project PASS_REGULAR_EXPRESSION "Previews: 1 views, [1-9][0-9]+ triangles"
                     FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(project_third_person PROPERTIES FIXTURES_SETUP tp_project)
add_test(NAME project_first_person COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/FP --template 2 --play --bench 3 --min-fps 200)
# The Quality mode's project (Cinematic: twice the rays, finer GI cache) played above 90 fps (its target: 90-120)
add_test(NAME project_quality COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/QTP --render-mode quality --template 1 --play --bench 3 --min-fps 90)
add_test(NAME project_ar COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/ARP --template 8 --play --bench 3 --min-fps 200) # Augmented Reality: BP_ARPlacer (Event Touch, AR Line Trace, Spawn) compiles and plays
add_test(NAME project_vr COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/VRP --template 9 --play --bench 3 --min-fps 200) # (Q25) Virtual Reality: BP_VRPawn (Event On VR Teleported, Play Haptic Effect) compiles and plays, the Grabbables, the socket, the door and the lever
# (Q25) the VR template packaged and played on the simulated headset: a cube grabbed, held (the player's capsule not pushed
# by it), thrown, landing on the floor (vr_pawn_script); the template's level saved, then packaged from its project
add_test(NAME vr_room_save COMMAND realengine --new-project ${CMAKE_BINARY_DIR}/test_projects/VRG --template 9 --save ${CMAKE_BINARY_DIR}/test_projects/VRG/Content/VRRoom.rlevel --bench 1)
set_tests_properties(vr_room_save PROPERTIES FIXTURES_SETUP vr_room_saved)
add_test(NAME vr_room_package COMMAND realengine --project ${CMAKE_BINARY_DIR}/test_projects/VRG/VRG.rproject --open ${CMAKE_BINARY_DIR}/test_projects/VRG/Content/VRRoom.rlevel --package ${CMAKE_BINARY_DIR}/test_game_vr --bench 1)
set_tests_properties(vr_room_package PROPERTIES FIXTURES_REQUIRED vr_room_saved FIXTURES_SETUP vr_room_pkg)
add_test(NAME vr_room_played COMMAND ${CMAKE_BINARY_DIR}/test_game_vr/VRRoom/VRRoom.exe --validation --windowed --vr-sim --vr-script --bench 5)
set_tests_properties(vr_room_played PROPERTIES FIXTURES_REQUIRED vr_room_pkg ENVIRONMENT "REALENGINE_XR_SIM_ONLY=1" PASS_REGULAR_EXPRESSION "VR script: thrown .* PASS" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (the projects made fresh: the area run alone found ARP / VRP left by an earlier run and "could not create the project")
set_tests_properties(project_ar project_vr vr_room_save PROPERTIES FIXTURES_REQUIRED projects)
add_test(NAME project_top_down COMMAND realengine --new-project ${CMAKE_BINARY_DIR}/test_projects/TD --template 3 --play --bench 3 --min-fps 200)
# The demo games (demo_games.inc): made, played; their Blueprints compile and run (the horror's volumetric fog and
# shadowed flashlight in closed rooms: ~170 fps at High)
# (Oakvale played by autoplay: the quest, the three jobs, the shop; an open village at Medium: ~166 fps, the sun's traced
# shadow over the whole view its biggest cost)
add_test(NAME demo_rpg COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoRPG --template 5 --play --scalability 1 --bench 6 --min-fps 150)
set_tests_properties(demo_rpg PROPERTIES ENVIRONMENT "REALENGINE_DEMO_AUTOPLAY=1" PASS_REGULAR_EXPRESSION "RPG: quest step 4")
# (the clinic played through by autoplay: the fuses, the clock, the breakers, the gate; a full scene now: 180 like the others)
add_test(NAME demo_horror COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoHorror --template 6 --play --scalability 1 --bench 8 --min-fps 180)
set_tests_properties(demo_horror PROPERTIES ENVIRONMENT "REALENGINE_DEMO_AUTOPLAY=1" PASS_REGULAR_EXPRESSION "Horror: .*escaped")
add_test(NAME demo_fighting COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoFight --template 7 --play --bench 6 --min-fps 180)
# (the fight played by both sides: REALENGINE_DEMO_AUTOPLAY gives you the rival's mind; blows land, montages play)
set_tests_properties(demo_fighting PROPERTIES ENVIRONMENT "REALENGINE_DEMO_AUTOPLAY=1" PASS_REGULAR_EXPRESSION "Fight: round")
# Their light standing still on Epic (notas-dev/gi.md, 2026-11-03): every mesh within reach on the surface cache (the
# ones left on the probes flickered: the RPG's houses, the horror's walls) and the screen's clouds without the jitter's
# noise. Before: RPG 0.46% (85 of 209 charts), horror 6 of 67 charts (its editor view at night, as reported: 8 s, the cache
# warmed up)
add_test(NAME demo_rpg_flicker COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoRPGFlicker --template 5
         --scalability 3 --cam -300,-600,250,60,-8 --bench 3 --max-flicker 0.3 --cache-all --min-fps 70) # (Oakvale at Epic with all 572 charts: 77-110 fps; flicker ~0.24%: textured grass, 10 people idling in view)
add_test(NAME demo_horror_flicker COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoHorrorFlicker --template 6
         --scalability 3 --bench 8 --max-flicker 0.2 --cache-all --min-fps 100) # (the clinic at Epic with every chart: ~120 fps; the demo plays at Medium)
set_tests_properties(project_third_person project_first_person project_quality project_top_down demo_rpg demo_horror demo_fighting demo_rpg_flicker
                     demo_horror_flicker PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL")
# (2026-11-19) the abandoned metro (--scene metro, notas-dev/demos.md): the CC0 hall made into a new project at High,
# from the reference's view (the sun's shafts through the broken roof, the train, the weeds): its level saved, the plants
# painted, no validation error, > 95 fps (with the validation layers: ~110-115; without: ~160-180 at 1440p on the 3060)
add_test(NAME demo_metro COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/DemoMetro --template 2 --scene metro
         --scalability 2 --bench 6 --min-fps 95)
set_tests_properties(demo_metro PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "Metro saved: "
                     FAIL_REGULAR_EXPRESSION "FAIL|VUID|DEVICE_LOST")
# An OBJ (two objects, quads, negative indices, an MTL with options before a texture whose name has a space) into a new
# project: the assets it must make (notas-dev/importar-tlender.md)
add_test(NAME import_obj COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/OBJ --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/test.obj --bench 1)
set_tests_properties(import_obj PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "3 meshes, 3 materials \\(1 textures, 0 nodes skipped\\), 0 skeletal meshes, 0 animations, 3 actors")
# glTF (tests/data/import/make_gltf.js writes them): a .glb with an embedded image, a hierarchy, a mesh on two nodes of
# the same name, a primitive without indices and one without a material, a spot light, a skinned body over two joints
# with an animation and a mesh under a joint (a Skeletal Mesh of 3 parts); a .gltf with a data URI buffer, an external
# image ("%20"), a triangle strip bent by a morph target at its default weight and a camera
add_test(NAME import_glb COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/GLB --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/test.glb --bench 1)
add_test(NAME import_gltf COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/GLTF --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/test.gltf --bench 1)
set_tests_properties(import_glb PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "7 meshes, 3 materials \\(3 textures, 0 nodes skipped\\), 1 skeletal meshes, 1 animations, 6 actors, 1 lights, [12] with vertex colors") # (its skin: one per material, weighted per vertex)
set_tests_properties(import_gltf PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "1 meshes, 1 materials \\(1 textures, 0 nodes skipped\\), 1 skeletal meshes, 0 animations, 1 actors, 1 morph targets, 1 cameras") # (the ribbon's morph target kept: a Skeletal Mesh of one bone)
# FBX (Blender 5.0 exports, from the Tlender's tests): an armature (scaled 100, as Blender writes it) deforming a body
# with a hat under a bone and an action; a cube whose texture is embedded in the file
add_test(NAME import_fbx_rig COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/FBXRIG --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/rig.fbx --bench 1)
# the skin played (notas-dev/animacao.md "Skinning suave"): bent on the GPU every frame its action moves it, its BLAS refit
add_test(NAME skin_gpu COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/SKIN --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/rig.fbx --play-eject --cam 0,600,150,270,-5 --bench 2 --min-fps 200)
set_tests_properties(skin_gpu PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|VUID"
                     PASS_REGULAR_EXPRESSION "Skins: 1, bent last frame 1")
# (2026-11-23) Texture names to their maps (mat_map_slot) and several dropped on a Principled BSDF, linked by them
add_test(NAME mat_autolink COMMAND realengine --mat-autolink-test)
# (2026-11-23) Organize Nodes (graph_layout.c): no overlaps, wires left to right, only the selection, a tree's order
add_test(NAME graph_layout COMMAND realengine --graph-layout-test)
set_tests_properties(graph_layout PROPERTIES PASS_REGULAR_EXPRESSION "graph layout: .*: ok" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(mat_autolink PROPERTIES PASS_REGULAR_EXPRESSION "autolink: .*: ok" FAIL_REGULAR_EXPRESSION "FAIL")
# (2026-11-23) The textures around an import (every importer: OBJ here, FBX/glTF/.tlend the same way): a texture named
# where it isn't (another drive, a .tga) found as a .png in ../textures; a material with none given its maps by its name
# (M_Barrel: Barrel_BaseColor, Barrel_Normal, Barrel_ORM): 4 textures
add_test(NAME import_near COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/NEAR --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/near/models/near.obj --bench 1)
set_tests_properties(import_near PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "2 meshes, 2 materials \\(4 textures, 0 nodes skipped\\)")
# (2026-11-23) A folder from Explorer, all of it (content_import_folder): Props/Crate (an OBJ, its texture in ./textures
# used where it was mirrored), Props/Barrel (its maps by its material's name beside it), a sound, a loose decal texture,
# a .blend (left out, said so); 7 folders mirrored
add_test(NAME import_folder COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/FOLDER --template 0
         --import-folder ${CMAKE_SOURCE_DIR}/tests/data/import/folder --bench 1)
set_tests_properties(import_folder PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not|Import lost"
                     PASS_REGULAR_EXPRESSION "Import folder .*: 7 folders, 2 models, 5 textures, 1 sounds, 1 unsupported, 2 skipped")
# (2026-11-23) A Static Mesh's sockets (saved, read back, on a turned and scaled actor) and its material saved from the viewer
add_test(NAME rig_detect COMMAND realengine --rig-test) # (2026-11-23) the rig's roles on Mixamo, Rigify, UE, no names, CC
set_tests_properties(rig_detect PROPERTIES PASS_REGULAR_EXPRESSION "rig_detect: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME rig_fit COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/RIGFIT --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/humanoid.glb --rig-fit-test --bench 1) # (2026-11-23) the bodies as thick as the skin
set_tests_properties(rig_fit PROPERTIES PASS_REGULAR_EXPRESSION "rig fit: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME preload COMMAND realengine --preload-test ${CMAKE_BINARY_DIR}/preload_test) # (2026-11-23) levels preloaded at a speed, swapped in
set_tests_properties(preload PROPERTIES PASS_REGULAR_EXPRESSION "preload: ok" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME mesh_sockets COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/MSOCK --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/near/models/near.obj --mesh-socket-test --bench 1)
set_tests_properties(mesh_sockets PROPERTIES FIXTURES_REQUIRED projects PASS_REGULAR_EXPRESSION "mesh sockets: .*: ok" FAIL_REGULAR_EXPRESSION "FAIL|Could not")
add_test(NAME import_fbx_embed COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/FBXEMB --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/embed.fbx --bench 1)
# FBX lights and cameras (tests/data/import/make_fbx_lights.py, Blender 5.0): a cube, a spot, a sun and a camera
add_test(NAME import_fbx_lights COMMAND realengine --validation --new-project ${CMAKE_BINARY_DIR}/test_projects/FBXLIGHTS --template 0
         --import ${CMAKE_SOURCE_DIR}/tests/data/import/lights.fbx --bench 1)
set_tests_properties(import_fbx_lights PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "1 meshes, 0 materials \\(0 textures, 0 nodes skipped\\), 0 skeletal meshes, 0 animations, 1 actors, 2 lights, 1 cameras")
set_tests_properties(import_fbx_rig PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "1 meshes, 0 materials \\(0 textures, 0 nodes skipped\\), 1 skeletal meshes, 1 animations, 1 actors") # (one skin: the body weighted per vertex, the hat on its bone)
set_tests_properties(import_fbx_embed PROPERTIES FIXTURES_REQUIRED projects FAIL_REGULAR_EXPRESSION "FAIL|Could not"
                     PASS_REGULAR_EXPRESSION "1 meshes, 1 materials \\(1 textures, 0 nodes skipped\\), 0 skeletal meshes, 0 animations, 1 actors")
add_test(NAME anim_crowd COMMAND realengine --scene anim1000 --play --bench 3 --min-fps 80)
# Blend Space 2D weights, two bone IK, blending N poses (the math); Foot IK: a Mannequin with its left foot over a 25 cm
# step, played: that foot rises onto it, the other stays, the pelvis stays (notas-dev/animacao.md)
add_test(NAME anim_math COMMAND realengine --anim-selftest)
# The asset previews on the GPU (notas-dev/editor.md): the Skeletal Mesh Editor and an Anim Blueprint's Viewport (its graph
# playing the Mannequin), validated, drawn every frame, the editor still above 300 fps
add_test(NAME preview_skeletal COMMAND realengine --validation --open-asset /Content/Characters/Mannequin/SK_Mannequin.rskm --bench 3 --min-fps 200)
add_test(NAME preview_anim_bp COMMAND realengine --validation --open-viewport /Content/Characters/Mannequin/ABP_Mannequin.rbp --bench 3 --min-fps 200)
# (Q7) the asset viewport (notas-dev/editor.md "Viewport de asset"): every editor with a 3D view (Static Mesh, Skeletal
# Mesh, Skeleton, Animation, Montage, Blend Space, Anim Blueprint, Physics Asset, IK Rig, Material, Particles, Vehicle,
# Cloth) opened on an example, its GPU picture read back in each of the 13 view modes with the model's pixels counted (not
# the bones'); a socket made, moved, saved and read back with its preview mesh drawn; the cost with 1 and 3 editors open
add_test(NAME asset_viewport COMMAND realengine --validation --asset-viewport-test)
set_tests_properties(asset_viewport PROPERTIES PASS_REGULAR_EXPRESSION "Asset viewport test: OK" FAIL_REGULAR_EXPRESSION "FAIL|VUID" TIMEOUT 300)
# (Q23, notas-dev/auditoria-artista.md) the artist tools' fixes: the .hdr decoder, the textures' kinds, the Static Mesh
# viewer's vertex count (no window); the Texture viewer's .hdr, its thumbnail and the viewer following the file, an .exr's reason
add_test(NAME artist_selftest COMMAND realengine --artist-selftest)
set_tests_properties(artist_selftest PROPERTIES PASS_REGULAR_EXPRESSION "Artist selftest: OK" FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 60)
add_test(NAME artist_viewer COMMAND realengine --validation --artist-test)
set_tests_properties(artist_viewer PROPERTIES PASS_REGULAR_EXPRESSION "Artist test: OK" FAIL_REGULAR_EXPRESSION "FAIL|VUID" TIMEOUT 120)
add_test(NAME anim_foot_ik COMMAND realengine --validation --scene footik --play --bench 2 --min-fps 200)
# crowds (notas-dev/multidao.md): 2000 agents close in on the player along the flow field above 200 fps; 20 000 above 100
add_test(NAME crowd COMMAND realengine --validation --scene crowd2000 --play --bench 4 --min-fps 200)
add_test(NAME crowd_20000 COMMAND realengine --scene crowd20000 --play --bench 4 --min-fps 100)
set_tests_properties(crowd crowd_20000 PROPERTIES PASS_REGULAR_EXPRESSION "Crowd: [0-9]+ agents" FAIL_REGULAR_EXPRESSION "FAIL")
# (Q27.1) a city's 10 000 pedestrians and a Crowd Alarm (a shot) among them at 1.5 s: the frames of the second after it within
# 1.5x the median before, at most 64 promoted to panic a frame, 90% of them reached by the wave, 200 fps
add_test(NAME crowd_panic COMMAND realengine --validation --scene panic10000 --play --bench 4 --min-fps 200)
set_tests_properties(crowd_panic PROPERTIES PASS_REGULAR_EXPRESSION "Crowd panic: 10000 agents" FAIL_REGULAR_EXPRESSION "FAIL")
# (Q27.2) a city's grid (8 x 8 junctions, 112 two-way roads, signals) and ~2000 cars by the IDM: queues at the reds
# without overlaps, no red run with room to stop, the junctions crossed, <= 1 ms of CPU, 200 fps
# (Q27.12) the GI fallback without rays (Low, outdoors: a carport, a block, a wall): its AO volume built, 200 fps
add_test(NAME gi_fallback COMMAND realengine --validation --no-rt --scalability 0 --scene gi_fallback --bench 4 --min-fps 200)
set_tests_properties(gi_fallback PROPERTIES PASS_REGULAR_EXPRESSION "GI fallback: built" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q27.11) the locomotion styles on Motion Matching: switched while walking without a pop, the tired one bows, <= 0.01 ms
add_test(NAME loco_styles COMMAND realengine --validation --scene loco_styles --play --bench 12 --min-fps 200)
set_tests_properties(loco_styles PROPERTIES PASS_REGULAR_EXPRESSION "Locomotion styles: the head" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (2026-11-23) root motion of montages, Motion Warping to a target, a paired montage in step (anim_warp.inc)
add_test(NAME warp COMMAND realengine --validation --scene warp --play --bench 7)
set_tests_properties(warp PROPERTIES PASS_REGULAR_EXPRESSION "warp: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME traversal COMMAND realengine --validation --scene traversal --play --bench 7) # (2026-11-23: Try Traversal)
add_test(NAME anim_warping COMMAND realengine --validation --scene anim_warping --play --bench 12) # (2026-11-23: Orientation / Stride Warping)
set_tests_properties(anim_warping PROPERTIES PASS_REGULAR_EXPRESSION "anim warping: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME combo COMMAND realengine --validation --scene combo --play --bench 7) # (2026-11-23: montage windows)
set_tests_properties(combo PROPERTIES PASS_REGULAR_EXPRESSION "combo: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
add_test(NAME cover COMMAND realengine --validation --scene cover --play --bench 6) # (2026-11-23: third-person cover)
set_tests_properties(cover PROPERTIES PASS_REGULAR_EXPRESSION "cover: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
set_tests_properties(traversal PROPERTIES PASS_REGULAR_EXPRESSION "traversal: ok" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q27.10) the city's routine: the shop's visitors by its hours, a crime's witness calls the police
add_test(NAME city_routine COMMAND realengine --validation --scene city_routine --play --bench 10 --min-fps 200)
set_tests_properties(city_routine PROPERTIES PASS_REGULAR_EXPRESSION "City routine: shop closed" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q27.9) 200 crowd agents, each its own skin, clothes and build: nearly all distinct, 200 fps
add_test(NAME npc_variety COMMAND realengine --validation --scene npc_variety --play --bench 4 --min-fps 200)
set_tests_properties(npc_variety PROPERTIES PASS_REGULAR_EXPRESSION "NPC variety: 200 agents" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q27.8) the city at night: a retroreflective sign shines back at a headlight beside the eye, not at one off to the side
add_test(NAME city_night COMMAND realengine --validation --scene city_night --bench 7 --min-fps 200)
set_tests_properties(city_night PROPERTIES PASS_REGULAR_EXPRESSION "City night: the headlight" FAIL_REGULAR_EXPRESSION "FAIL|VUID")
# (Q27.6) the regional weather: dry here, a storm 1.5 km away raining, its ground keeping its water 10 minutes after
add_test(NAME weather_regional COMMAND realengine --validation --scene weather_regional --bench 7 --min-fps 200)
set_tests_properties(weather_regional PROPERTIES PASS_REGULAR_EXPRESSION "Weather regional: here" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME weather_field COMMAND realengine --weather-field-test)
set_tests_properties(weather_field PROPERTIES PASS_REGULAR_EXPRESSION "weather field: stormy" FAIL_REGULAR_EXPRESSION "FAIL")
# (Q27.4) 50 cars crashing head-on at their corners: none through another, dented, crushed fronts weaker, bent wheels pull, <= 1 ms
add_test(NAME vehicle_crash COMMAND realengine --validation --scene vehicle_crash --play --bench 4 --min-fps 200)
set_tests_properties(vehicle_crash PROPERTIES PASS_REGULAR_EXPRESSION "Vehicle crash: 50 cars" FAIL_REGULAR_EXPRESSION "FAIL")
# (Q27.3) 100 heads with dynamic hair: the strand segments under the frame's budget, at most the 16 nearest simulated, 200 fps
add_test(NAME hair_crowd COMMAND realengine --validation --scene hair_crowd --play --bench 4 --min-fps 200)
set_tests_properties(hair_crowd PROPERTIES PASS_REGULAR_EXPRESSION "Hair crowd: 100 grooms" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME traffic_queue COMMAND realengine --validation --scene traffic --play --bench 4 --min-fps 200)
set_tests_properties(traffic_queue PROPERTIES PASS_REGULAR_EXPRESSION "Traffic: [0-9]+ cars" FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(anim_crowd PROPERTIES PASS_REGULAR_EXPRESSION "Animation: 1000 skeletal meshes" FAIL_REGULAR_EXPRESSION "FAIL")
add_test(NAME debug_viewer COMMAND realengine --validation --scene house --bp-walkview --debug-viewer --bench 2 --min-fps 200)
set_tests_properties(debug_viewer PROPERTIES PASS_REGULAR_EXPRESSION "Debug Viewer: Meshes ~[0-9.]+ ms GPU [(][1-9]" FAIL_REGULAR_EXPRESSION "FAIL")
# 300: the package carries the Character's Skeletal Mesh (the Mannequin, 28 parts close to the camera: +0.4 ms GPU)
add_test(NAME game_blueprints COMMAND ${RE_TEST_GAME}/bp/BpLevel/BpLevel.exe --validation --bench 3 --min-fps 200) # (motion blur of the walking camera, notas-dev/render.md)
set_tests_properties(game_blueprints PROPERTIES FIXTURES_REQUIRED game_bp PASS_REGULAR_EXPRESSION "Hello from the Character" FAIL_REGULAR_EXPRESSION "FAIL")

if(MSVC)
    foreach(T re_core re_render realengine realengine_game re_tests re_test_i18n re_test_jobs re_test_ui_popups realengine_updater re_test_audio)
        target_compile_options(${T} PRIVATE /W3 /utf-8 /arch:AVX2 $<$<CONFIG:Release>:/O2 /Oi /GS- /Zi>)
        target_compile_definitions(${T} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
    target_compile_options(cimgui PRIVATE /utf-8 $<$<CONFIG:Release>:/Zi>)
    # A window app that starts at main() (no console; the log goes to %APPDATA%\RealEngine\realengine.log)
    foreach(T realengine realengine_game)
        target_link_options(${T} PRIVATE /ENTRY:mainCRTStartup "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")
    endforeach()
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT realengine)
endif()

# The icon and the version in the executable
if(WIN32)
    set(RE_ICON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data/branding/icon.ico)
    string(REPLACE "/" "\\\\" RE_ICON_FILE "${RE_ICON_FILE}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/data/branding/realengine.rc.in ${RE_GENERATED_DIR}/realengine.rc @ONLY)
    target_sources(realengine PRIVATE ${RE_GENERATED_DIR}/realengine.rc)
    target_sources(realengine_game PRIVATE ${RE_GENERATED_DIR}/realengine.rc) # (the packaged game: the engine's icon until the project has its own)
    # (2026-11-23, Real Engine -> Rivora Engine) the targets keep their names (the tests' COMMAND realengine runs rivora.exe);
    # the files get the new one. realengine.exe: the launcher the older installs' shortcuts and updater open
    set_target_properties(realengine PROPERTIES OUTPUT_NAME rivora)
    set_target_properties(realengine_game PROPERTIES OUTPUT_NAME rivora_game)
    set_target_properties(realengine_updater PROPERTIES OUTPUT_NAME rivora_updater)
    add_executable(realengine_legacy WIN32 src/legacy_launcher.c ${RE_GENERATED_DIR}/realengine.rc)
    set_target_properties(realengine_legacy PROPERTIES OUTPUT_NAME realengine)
    add_dependencies(realengine realengine_legacy)
    set_property(SOURCE ${RE_GENERATED_DIR}/realengine.rc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/branding/icon.ico)
endif()
# The Blueprint debugger (bp_debug.h, notas-dev/blueprints.md "Debugger"): a breakpoint stops BeginPlay (the world waits),
# a pin and a variable read, Step Into / Over, a script's line breakpoint and its step by lines, a Tick's breakpoint
# traced, Continue; the VM's cost with no breakpoint and with one elsewhere (bp_debug_test.c)
add_test(NAME bp_debugger COMMAND realengine --validation --bp-debug-test --bench 4)
set_tests_properties(bp_debugger PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|VUID" PASS_REGULAR_EXPRESSION "bp-debug test: OK")

# The suite's areas (a LABEL per test), the quick kit (tools/suite.ps1) and the GPU lock: after every add_test above
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/suite_areas.cmake)

# (B15a) Shipping builds: link-time code generation over the engine's libraries and both executables (the release
# script turns it on; a plain build keeps quick links)
option(REALENGINE_LTO "Link-time optimization (MSVC /GL + /LTCG) for the release build" OFF)
if(REALENGINE_LTO)
    set_property(TARGET re_core re_render realengine realengine_game PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
