# The suite's areas and the quick kit (notas-dev/testes.md). Included at the end of CMakeLists.txt, after every add_test.
#
# Every test gets the LABEL of its area (the name of its note in notas-dev/, so `ctest -L fisica` runs physics whole);
# the chosen few also get "quick": at least one per area, the cheapest that goes through the area's main path, so
# `tools/suite.ps1` (= ctest -L quick) touches every area in ~4 min instead of the whole suite's ~20.
# The tests that open no window (unit tests, self-tests: "cpu") run side by side; every other one takes the "gpu" lock,
# so `ctest -j N` never puts two windows on the GPU at once (their fps and flicker limits stay what they measure alone).
#
# A new test: add its name to its area below (and to RE_QUICK only if its area has nothing quicker, to RE_CPU if it opens
# no window). The `suite_areas` test fails while a test has no area or an area has no quick test.

get_property(RE_ALL_TESTS DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY TESTS)
set(RE_AREAS "")
set(RE_AREA_TESTS "")

macro(re_area AREA)
    list(APPEND RE_AREAS ${AREA})
    set(RE_AREA_${AREA} "")
    foreach(t ${ARGN})
        if(t IN_LIST RE_ALL_TESTS) # (the vendors' upscalers and the assistant exist only with their SDK / Python)
            set_property(TEST ${t} APPEND PROPERTY LABELS ${AREA})
            list(APPEND RE_AREA_${AREA} ${t})
            list(APPEND RE_AREA_TESTS ${t})
        endif()
    endforeach()
endmacro()

re_area(editor ecs gizmo_ui start_pipelines start_pipelines_validation ui_popups ui_tips theme theme_literals theme_contrast dock_targets editor_close_clean editor_close intro_window look_migration tips_audit icon_sheet graph_layout editor_fps editor_ortho editor_ortho_front object_cost construction_editor
        source_control plugins plugin_template skeletal_gizmo_move asset_viewport)
re_area(servicos gpu_marks update_download hang_watchdog)
re_area(jobs jobs)
re_area(idiomas i18n)
re_area(audio audio_mixer audio_play)
re_area(gi gi_house gi_house_thin gi_house_movable gi_bake gi_no_rt gi_house_sw far_probes house_walk
        ingame_package_loading ingame_package_progressive game_bake_loading game_bake_progressive
        ghost_realistic ghost_medium ghost_photoreal stable_pan stable_pan_photoreal gi_fallback) # (Q20: notas-dev/gi-estavel.md)
re_area(luz shadow_filter spec_leak spec_leak_epic spec_leak_low spec_leak_nort many_lights many_lights_restir
        weather_sky shadow_ghost cine_camera fog_volume volume_vdb)
re_area(render contact_shadows color_grading_lut terror_filters terror_exposure terror_channels ssr async_compute play_flicker shader_spill skin_sss skin_sss_low eyes
        hair hair_low hair_dynamic hair_crowd skin_churn denoiser_house denoiser_area render_style_stylized render_style_photoreal render_style_video vrs
        view2 view2_pt path_trace_play heavy_mesh cluster_lod translucent splat light_function toon toon_msaa toon_low
        camshake_determinism fp_autofocus) # (Q12: the Camera Shakes, the autofocus)
re_area(materiais materials material_lobes material_lobes_low masked_lobes masked_lobes_low decals decals_low mat_pins
        texture_streaming transmission_medium interior_mapping city_night mated_ui emissive_light emissive_light_off) # (mated_ui: the Material Editor by injected input)
re_area(udim udim_format udim_import udim_seams udim_stream) # (Q18, notas-dev/udim.md)
re_area(rt-clusters rt_clusters rt_classic)
re_area(tessellation tess_rocks tess_seams tess_low tess_budget) # (Q17: Nanite Tessellation)
re_area(visao-distante vista vista_low vista_pop vista_walk) # (Q21: the distant view)
re_area(escala scale_rt lights_scale)
re_area(mundos-grandes world_partition wp_fast)
re_area(orcamento budget_house budget_rpg budget_heavy budget_heavy_dynres)
re_area(upscaling upscale upscaler_dlss upscaler_fsr upscaler_xess)
re_area(debug-viewer debug_viewer trace_record trace_window)
re_area(jogo game_package game_fps game_package_rpak game_hdr10 game_rpak_runs game_path_tracing game_first_run
        game_blueprints)
re_area(marca game_package_icon game_icon_runs)
re_area(xr ar arcore vr_sim ar_sim project_ar vr_selftest vr_base_hand vr_hand_package vr_hand_played vr_quest_sim project_vr vr_room_save vr_room_package vr_room_played mr_selftest mr_sim)
re_area(tecido cloth_mesh_graph cloth_mesh cloth_scene gpu_marks softbody_clean softbody_lattice softbody_volume softbody_sofa) # (gpu_marks runs the cloth scene with validation; Q9 the soft bodies)
re_area(agua water rain_scene rain_lens rain_marks lake_scene snow_scene ocean_scene river underwater rain_clear_drops weather_regional weather_field)
re_area(veiculos vehicle_selftest vehicle_from_mesh vehicles_scene vehicle_crash)
re_area(online net_protocol net_session pie_multiplayer net_session_advanced)
re_area(blueprints blueprints graph_menu bp_graph_menu bp_debugger gameplay_tags bp_viewport_character ingame_sweep)
re_area(particulas particles_selftest particles_scene particles_5m fire)
re_area(traces scene_queries trace_nodes trace_bench)
re_area(ikrig rig_detect rig_fit ikphys_selftest ikphys_ragdoll ikphys_ragdoll_jitter active_ragdoll ragdoll_limits ragdoll_active dyn_bones cloth_on_body getup_q10 ragdoll_wounds)
re_area(procedural procgen_selftest procgen_scene procgen_steady procgen_facility)
re_area(pcg pcg_selftest pcg_forest pcg_stream) # (Q15)
re_area(sequencer sequencer cinematics)
re_area(android mobile_selftest touch_selftest)
re_area(fisica constraints char_physics physics physics_stress physics_stress_shapes physics_shapes phys_stack phys_stack_awake phys_ccd
        phys_joints phys_heavy phys_crates softbody_rigid) # (Q9: a box on a soft cushion)
re_area(ia ai ai_squad ai_squad_mm navmesh ai_senses)
re_area(importar-tlender import_tlend_shapekeys import_obj import_glb import_gltf import_fbx_rig import_fbx_embed
        import_fbx_lights delight import_tlend_hair import_obj_hair import_tlend_q19 import_tlend_q19_baked import_tlend_engine import_tlend_creases import_tlend_bone_parent import_tlend_bone_relative import_rasset import_near import_folder mat_autolink mesh_sockets) # (Q19: the Tlender whole; #164)
re_area(animacao morph_targets skin_gpu anim_packs autorig mm_startstop mm_examples mm_longtake mm_authored mm_jump anim_crowd
        anim_math preview_skeletal preview_anim_bp anim_foot_ik ai_squad_mm socket_offset
        fp_bob_sync fp_land_recoil fps_camera loco_styles warp traversal anim_warping combo cover) # (Q12: the First Person Camera)
re_area(multidao crowd crowd_20000 crowd_panic npc_variety city_routine)
re_area(transito traffic_queue)
re_area(inventario inventory)
re_area(interacao interaction)
re_area(dialogo dialogue)
re_area(missoes quest)
re_area(regras-de-jogo rules)
re_area(habilidades ability)
re_area(hud hud)
re_area(niveis levels levels_lighting_sum levels_remove levels_panel preload)
re_area(content-browser asset_picker artist_selftest artist_viewer) # (Q23: the Texture viewer, thumbnails, vertex count)
re_area(pausa-opcoes pause_options pause_options_reload)
re_area(salvar-progresso save_progress)
re_area(saude-dano health)
re_area(landscape landscape landscape_paint landscape_heightmap)
re_area(prototipagem proto)
re_area(splines spline_eval spline_rail_river cable_hang splines_scene) # (Q14)
re_area(umg umg)
re_area(async async)
re_area(assistente assistant_pack assistant)
re_area(scripts scripts)
re_area(projetos projects_clean project_third_person project_first_person project_quality project_top_down)
re_area(demos demo_rpg demo_horror demo_fighting demo_rpg_flicker demo_horror_flicker demo_metro)

# The quick kit (fixtures come along by themselves: game_fps brings game_package, the projects' tests projects_clean)
set(RE_QUICK
    ecs ui_popups ui_tips theme theme_literals editor_fps gpu_marks jobs i18n audio_mixer audio_play gi_house spec_leak
    render_style_photoreal materials mat_pins rt_clusters lights_scale world_partition budget_house upscale debug_viewer
    game_fps game_package_icon ar arcore cloth_mesh_graph water rain_scene vehicle_selftest vehicle_from_mesh
    net_protocol blueprints graph_menu gameplay_tags particles_selftest particles_scene ikphys_selftest procgen_selftest
    procgen_facility sequencer mobile_selftest touch_selftest constraints ai import_gltf anim_math anim_foot_ik crowd
    inventory interaction dialogue quest rules ability hud levels pause_options save_progress health landscape_heightmap proto
    umg async assistant_pack assistant scripts project_third_person demo_fighting asset_picker cable_hang pcg_selftest
    udim_seams # (Q18)
    vista_pop # (Q21)
    traffic_queue # (Q27.2)
    artist_selftest # (Q23)
    tess_seams # (Q17)
    trace_nodes) # (2026-11-19: the traces, Trace Complex, async)

# Tests that open no window (no Vulkan device): they run beside the others
set(RE_CPU
    ecs audio_mixer i18n jobs ui_popups ui_tips theme theme_literals theme_contrast net_protocol ar arcore cloth_mesh_graph softbody_lattice softbody_clean water particles_selftest
    graph_menu procgen_selftest mobile_selftest touch_selftest vehicle_selftest ikphys_selftest delight gameplay_tags
    mat_pins anim_math upscale assistant_pack update_download ragdoll_limits ragdoll_active dyn_bones ragdoll_wounds
    fp_bob_sync camshake_determinism fp_autofocus fp_land_recoil spline_eval spline_rail_river cable_hang pcg_selftest
    udim_format udim_import # (Q18)
    artist_selftest # (Q23)
    scene_queries) # (2026-11-19)

foreach(t ${RE_QUICK})
    if(t IN_LIST RE_ALL_TESTS)
        set_property(TEST ${t} APPEND PROPERTY LABELS quick)
    endif()
endforeach()
foreach(t ${RE_ALL_TESTS})
    if(t IN_LIST RE_CPU)
        set_property(TEST ${t} APPEND PROPERTY LABELS cpu)
    else()
        set_property(TEST ${t} APPEND PROPERTY RESOURCE_LOCK gpu)
    endif()
endforeach()

# The check: every test in an area, every area in the kit
set(RE_SUITE_PROBLEMS "")
foreach(t ${RE_ALL_TESTS})
    if(NOT t IN_LIST RE_AREA_TESTS)
        string(APPEND RE_SUITE_PROBLEMS " test '${t}' has no area;")
    endif()
endforeach()
foreach(a ${RE_AREAS})
    set(has_quick FALSE)
    foreach(t ${RE_AREA_${a}})
        if(t IN_LIST RE_QUICK)
            set(has_quick TRUE)
        endif()
    endforeach()
    if(NOT has_quick)
        string(APPEND RE_SUITE_PROBLEMS " area '${a}' has no quick test;")
    endif()
endforeach()
list(LENGTH RE_ALL_TESTS RE_N_TESTS)
list(LENGTH RE_AREAS RE_N_AREAS)
list(LENGTH RE_QUICK RE_N_QUICK)
if(RE_SUITE_PROBLEMS)
    message(WARNING "Suite areas (tools/suite_areas.cmake):${RE_SUITE_PROBLEMS}")
    add_test(NAME suite_areas COMMAND ${CMAKE_COMMAND} -E echo "FAIL (tools/suite_areas.cmake):${RE_SUITE_PROBLEMS}")
else()
    add_test(NAME suite_areas COMMAND ${CMAKE_COMMAND} -E echo
             "Suite areas: ${RE_N_TESTS} tests in ${RE_N_AREAS} areas, ${RE_N_QUICK} in the quick kit")
endif()
set_tests_properties(suite_areas PROPERTIES LABELS "quick;cpu" FAIL_REGULAR_EXPRESSION "FAIL")
