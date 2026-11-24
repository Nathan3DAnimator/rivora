// The project's settings (project.h): a JSON file, every key optional (an older project opens with the new defaults).
#include "project.h"
#include "audio_internal.h"
#include "demo_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

static ProjectSettings g_p;
static bool g_init;
static char g_file[600];

void project_defaults(ProjectSettings* p) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "MyProject");
    snprintf(p->version, sizeof p->version, "1.0.0");
    p->gravity_z = 980.0f;
    p->max_substeps = 4;
    p->solver_substeps = 4;
    p->physics_rate = 120;
    p->key_forward = 87, p->key_back = 83, p->key_left = 65, p->key_right = 68, p->key_jump = 32; // W S A D Space
    p->mouse_sensitivity = 0.15f;
    p->show_intro = true;
    p->game_scalability = 2;
    p->net_port = 7777, p->net_max_players = 16, p->net_tick_rate = 30, p->net_interp_ms = 100.0f, p->steam_cloud = true;
    p->ray_tracing = true, p->static_lighting = true, p->auto_exposure = true;
    p->upscaler = 0, p->upscale_mode = 0, p->dyn_res = false, p->dyn_res_fps = 100, p->dyn_res_min = 0;
    p->render_style = 1; // (Realistic: the look so far)
    p->stylized_bands = 4, p->stylized_softness = 0.0f, p->stylized_saturation = 1.3f; // (the Stylized look so far)
    p->stylized_shadow_tint[0] = p->stylized_shadow_tint[1] = p->stylized_shadow_tint[2] = 1.0f;
    p->stylized_outline = 0.0f, p->stylized_outline_depth = 0.04f, p->stylized_anim_fps = 0;
    p->film_grain = 1.0f, p->image_denoise = 0;
    p->shadow_denoise = 2, p->video_distortion = p->video_fringe = p->video_halation = p->video_vignette = 1.0f;
    p->vrs = false;
    p->async_compute = false;
    p->tsr_disocclusion = p->den_history_fix = true; // (Q20)
    snprintf(p->window_title, sizeof p->window_title, "My Game");
    p->resolution_w = 1920, p->resolution_h = 1080;
    p->audio_max_channels = 64, p->audio_master_volume = 1.0f;
    p->fullscreen = true, p->mouse_capture = true;
    p->android_version_code = 1, p->android_touch_controls = true, p->android_ar_occlusion = true;
    p->android_target_fps = 60, p->android_dynamic_res = true, p->android_auto_version = true;
    p->package_platform = -1;
    snprintf(p->android_icon_bg, sizeof p->android_icon_bg, "#101418");
    p->pause_menu = true;
    snprintf(p->pie_pause_key, sizeof p->pie_pause_key, "P");
    { // (Q25) VR: the Unreal VR Template's defaults (teleport and snap turns: the comfortable ones), the Quest's preset
        ProjectVr* v = &p->vr;
        v->pc = v->quest = true, v->refresh_hz = 90, v->seated_height = 120.0f, v->snap_angle = 45.0f, v->turn_speed = 90.0f;
        v->move_speed = 200.0f, v->teleport_range = 900.0f, v->vignette = 0.6f, v->hands = v->hand_tracking = v->laser = v->drag_locomotion = true;
        v->physical_hands = v->hand_objects = v->hand_push = v->held_collide = v->hide_body = true, v->head_collision = VR_HEAD_FADE, v->hand_mesh_scale = 1.0f;
        v->teleport_fade = 0.12f, v->turn_fade = false, v->grab_reach = 10.0f, v->distance_grab = true, v->throw_scale = 1.0f;
        v->haptics = 1.0f, v->laser_length = 1000.0f, v->menu_distance = 120.0f, v->menu_width = 128.0f, v->msaa = 4, v->headroom_fps = 150, v->safety_net = true, v->mirror_mode = VR_MIRROR_ONE_EYE, v->spectator_fov = 90.0f, v->spectator_smooth = 0.6f;
        v->grab_gesture = VR_GESTURE_FIST_OR_TOUCH, v->drag_gesture = VR_GESTURE_FIST;
        v->quality = VR_QUALITY_QUEST, v->foveation = 2, v->foveation_dynamic = true, v->render_scale = 1.0f;
        // (Q25b) mixed reality: off until a project turns it on; then the passthrough with the room in the sky's place, the
        // room loaded as actors, the real depth hiding the virtual, the shadows on the real floor
        v->mr_passthrough = true, v->mr_opacity = 1.0f, v->mr_contrast = v->mr_saturation = 1.0f;
        v->mr_tint[0] = 0.3f, v->mr_tint[1] = 1.0f, v->mr_tint[2] = 0.45f, v->mr_virtual_opacity = 1.0f;
        v->mr_scene = v->mr_scene_actors = v->mr_room_mesh = v->mr_ask_scan = v->mr_align = true;
        v->mr_occlusion = MR_OCC_DEPTH, v->mr_occlusion_soft = 4.0f, v->mr_hand_removal = true, v->mr_shadows = 0.6f;
        v->mr_light = MR_LIGHT_LEVEL, v->mr_ev100 = 0.0f, v->mr_hide_floor = true; // (the exposure automatic: only the virtual pixels metered)
    }
    p->autosave_slots = 3, p->save_thumbnail = true; // (no timed auto save, none at a level change, until set: a project opts in)
}

ProjectSettings* project(void) {
    if (!g_init) project_defaults(&g_p), g_init = true;
    return &g_p;
}

const char* project_file(void) { return g_file; }

void project_root(char* out, size_t cap) {
    if (g_file[0]) {
        snprintf(out, cap, "%s", g_file);
        char* s = strrchr(out, '/');
        char* b = strrchr(out, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        return;
    }
    if (!re_exe_dir(out, cap)) snprintf(out, cap, ".");
}

#define S(k, f) snprintf(p->f, sizeof p->f, "%s", json_get_str(r, k, p->f))
bool project_load(const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // a load, not a frame
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    JsonDoc doc;
    char err[160];
    if (!ok || !json_parse(t, (size_t)n, &doc, err, sizeof err)) {
        free(t);
        return false;
    }
    ProjectSettings* p = project();
    project_defaults(p);
    const JsonValue* r = &doc.root;
    S("name", name), S("company", company), S("version", version), S("description", description);
    S("editor_startup_map", editor_startup_map), S("game_default_map", game_default_map);
    S("game_mode", game_mode), S("default_pawn", default_pawn), S("player_controller", player_controller), S("hud", hud);
    S("game_state", game_state), S("player_state", player_state), S("game_instance", game_instance);
    p->gravity_z = (float)json_get_num(r, "gravity_z", p->gravity_z);
    p->max_substeps = json_get_int(r, "max_substeps", p->max_substeps);
    p->solver_substeps = json_get_int(r, "solver_substeps", p->solver_substeps);
    p->physics_rate = json_get_int(r, "physics_rate", p->physics_rate);
    p->key_forward = json_get_int(r, "key_forward", p->key_forward);
    p->key_back = json_get_int(r, "key_back", p->key_back);
    p->key_left = json_get_int(r, "key_left", p->key_left);
    p->key_right = json_get_int(r, "key_right", p->key_right);
    p->key_jump = json_get_int(r, "key_jump", p->key_jump);
    p->mouse_sensitivity = (float)json_get_num(r, "mouse_sensitivity", p->mouse_sensitivity);
    p->invert_mouse_y = json_get_bool(r, "invert_mouse_y", p->invert_mouse_y);
    snprintf(p->default_imc, sizeof p->default_imc, "%s", json_get_str(r, "default_imc", p->default_imc));
    p->game_scalability = json_get_int(r, "game_scalability", p->game_scalability);
    p->audio_max_channels = json_get_int(r, "audio_max_channels", p->audio_max_channels);
    p->audio_binaural = json_get_bool(r, "audio_binaural", p->audio_binaural);
    p->audio_master_volume = (float)json_get_num(r, "audio_master_volume", p->audio_master_volume);
    snprintf(p->android_package, sizeof p->android_package, "%s", json_get_str(r, "android_package", p->android_package));
    snprintf(p->android_keystore, sizeof p->android_keystore, "%s", json_get_str(r, "android_keystore", p->android_keystore));
    snprintf(p->android_key_alias, sizeof p->android_key_alias, "%s", json_get_str(r, "android_key_alias", p->android_key_alias));
    snprintf(p->android_key_pass_file, sizeof p->android_key_pass_file, "%s", json_get_str(r, "android_key_pass_file", p->android_key_pass_file));
    p->android_orientation = json_get_int(r, "android_orientation", p->android_orientation);
    p->android_version_code = json_get_int(r, "android_version_code", p->android_version_code);
    p->android_touch_controls = json_get_bool(r, "android_touch_controls", p->android_touch_controls);
    p->android_ar = json_get_bool(r, "android_ar", p->android_ar);
    p->android_ar_occlusion = json_get_bool(r, "android_ar_occlusion", p->android_ar_occlusion);
    p->android_quality = json_get_int(r, "android_quality", p->android_quality);
    p->android_target_fps = json_get_int(r, "android_target_fps", p->android_target_fps);
    p->android_dynamic_res = json_get_bool(r, "android_dynamic_res", p->android_dynamic_res);
    p->android_content_scale = (float)json_get_num(r, "android_content_scale", p->android_content_scale);
    { // (Q25) VR
        ProjectVr* v = &p->vr;
        v->start_in_vr = json_get_bool(r, "vr_start", v->start_in_vr);
        v->pc = json_get_bool(r, "vr_pc", v->pc), v->quest = json_get_bool(r, "vr_quest", v->quest);
        v->refresh_hz = json_get_int(r, "vr_refresh_hz", v->refresh_hz), v->tracking = json_get_int(r, "vr_tracking", v->tracking);
        v->seated_height = (float)json_get_num(r, "vr_seated_height", v->seated_height);
        v->locomotion = json_get_int(r, "vr_locomotion", v->locomotion), v->turn = json_get_int(r, "vr_turn", v->turn);
        v->snap_angle = (float)json_get_num(r, "vr_snap_angle", v->snap_angle), v->turn_speed = (float)json_get_num(r, "vr_turn_speed", v->turn_speed);
        v->move_speed = (float)json_get_num(r, "vr_move_speed", v->move_speed);
        v->teleport_range = (float)json_get_num(r, "vr_teleport_range", v->teleport_range);
        v->vignette = (float)json_get_num(r, "vr_vignette", v->vignette);
        v->hands = json_get_bool(r, "vr_hands", v->hands), v->hand_tracking = json_get_bool(r, "vr_hand_tracking", v->hand_tracking);
        v->laser = json_get_bool(r, "vr_laser", v->laser);
        v->drag_locomotion = json_get_bool(r, "vr_drag", v->drag_locomotion);
        v->physical_hands = json_get_bool(r, "vr_physical_hands", v->physical_hands), v->hand_push = json_get_bool(r, "vr_hand_push", v->hand_push);
        v->hand_objects = json_get_bool(r, "vr_hand_objects", v->hand_objects);
        v->held_collide = json_get_bool(r, "vr_held_collide", v->held_collide), v->hide_body = json_get_bool(r, "vr_hide_body", v->hide_body);
        v->head_collision = json_get_int(r, "vr_head_collision", v->head_collision);
        snprintf(v->hand_mesh[0], sizeof v->hand_mesh[0], "%s", json_get_str(r, "vr_hand_mesh_l", v->hand_mesh[0]));
        snprintf(v->hand_mesh[1], sizeof v->hand_mesh[1], "%s", json_get_str(r, "vr_hand_mesh_r", v->hand_mesh[1]));
        v->hand_mesh_scale = (float)json_get_num(r, "vr_hand_mesh_scale", v->hand_mesh_scale);
        json_get_floats(r, "vr_hand_mesh_rot", v->hand_mesh_rot, 3);
        v->dominant_hand = json_get_int(r, "vr_dominant_hand", v->dominant_hand), v->move_direction = json_get_int(r, "vr_move_direction", v->move_direction);
        v->teleport_fade = (float)json_get_num(r, "vr_teleport_fade", v->teleport_fade), v->turn_fade = json_get_bool(r, "vr_turn_fade", v->turn_fade);
        v->grab_reach = (float)json_get_num(r, "vr_grab_reach", v->grab_reach), v->distance_grab = json_get_bool(r, "vr_distance_grab", v->distance_grab);
        v->throw_scale = (float)json_get_num(r, "vr_throw_scale", v->throw_scale), v->haptics = (float)json_get_num(r, "vr_haptics", v->haptics);
        v->laser_length = (float)json_get_num(r, "vr_laser_length", v->laser_length);
        v->menu_distance = (float)json_get_num(r, "vr_menu_distance", v->menu_distance), v->menu_width = (float)json_get_num(r, "vr_menu_width", v->menu_width);
        v->msaa = json_get_int(r, "vr_msaa", v->msaa), v->headroom_fps = json_get_int(r, "vr_headroom_fps", v->headroom_fps), v->safety_net = json_get_bool(r, "vr_safety_net", v->safety_net);
        v->mirror_mode = json_get_int(r, "vr_mirror_mode", v->mirror_mode), v->spectator_fov = (float)json_get_num(r, "vr_spectator_fov", v->spectator_fov);
        v->spectator_smooth = (float)json_get_num(r, "vr_spectator_smooth", v->spectator_smooth);
        v->grab_gesture = json_get_int(r, "vr_grab_gesture", v->grab_gesture), v->drag_gesture = json_get_int(r, "vr_drag_gesture", v->drag_gesture);
        v->quality = json_get_int(r, "vr_quality", v->quality), v->foveation = json_get_int(r, "vr_foveation", v->foveation);
        v->foveation_dynamic = json_get_bool(r, "vr_foveation_dynamic", v->foveation_dynamic);
        v->spacewarp = json_get_bool(r, "vr_spacewarp", v->spacewarp);
        v->render_scale = (float)json_get_num(r, "vr_render_scale", v->render_scale);
        // (Q25b) mixed reality
        v->mr_enabled = json_get_bool(r, "mr_enabled", v->mr_enabled), v->mr_passthrough = json_get_bool(r, "mr_passthrough", v->mr_passthrough);
        v->mr_background = json_get_int(r, "mr_background", v->mr_background), v->mr_opacity = (float)json_get_num(r, "mr_opacity", v->mr_opacity);
        v->mr_style = json_get_int(r, "mr_style", v->mr_style), v->mr_brightness = (float)json_get_num(r, "mr_brightness", v->mr_brightness);
        v->mr_contrast = (float)json_get_num(r, "mr_contrast", v->mr_contrast), v->mr_saturation = (float)json_get_num(r, "mr_saturation", v->mr_saturation);
        json_get_floats(r, "mr_tint", v->mr_tint, 3), json_get_floats(r, "mr_edge", v->mr_edge, 4);
        v->mr_virtual_opacity = (float)json_get_num(r, "mr_virtual_opacity", v->mr_virtual_opacity);
        v->mr_scene = json_get_bool(r, "mr_scene", v->mr_scene), v->mr_scene_actors = json_get_bool(r, "mr_scene_actors", v->mr_scene_actors);
        v->mr_room_mesh = json_get_bool(r, "mr_room_mesh", v->mr_room_mesh), v->mr_ask_scan = json_get_bool(r, "mr_ask_scan", v->mr_ask_scan);
        v->mr_align = json_get_bool(r, "mr_align", v->mr_align), v->mr_occlusion = json_get_int(r, "mr_occlusion", v->mr_occlusion);
        v->mr_occlusion_soft = (float)json_get_num(r, "mr_occlusion_soft", v->mr_occlusion_soft), v->mr_hand_removal = json_get_bool(r, "mr_hand_removal", v->mr_hand_removal);
        v->mr_shadows = (float)json_get_num(r, "mr_shadows", v->mr_shadows), v->mr_light = json_get_int(r, "mr_light", v->mr_light);
        v->mr_ev100 = (float)json_get_num(r, "mr_ev100", v->mr_ev100), v->mr_windows_real = json_get_bool(r, "mr_windows_real", v->mr_windows_real);
        v->mr_hide_floor = json_get_bool(r, "mr_hide_floor", v->mr_hide_floor), v->mr_depth_flip = json_get_bool(r, "mr_depth_flip", v->mr_depth_flip);
        v->mr_unpremultiplied = json_get_bool(r, "mr_unpremultiplied", v->mr_unpremultiplied);
    }
    p->pause_menu = json_get_bool(r, "pause_menu", p->pause_menu);
    S("pause_menu_widget", pause_menu_widget), S("pie_pause_key", pie_pause_key), S("main_menu_map", main_menu_map);
    S("save_game_class", save_game_class);
    p->autosave_interval = (float)json_get_num(r, "autosave_interval", p->autosave_interval);
    p->autosave_slots = json_get_int(r, "autosave_slots", p->autosave_slots);
    p->autosave_on_level = json_get_bool(r, "autosave_on_level", p->autosave_on_level);
    p->save_thumbnail = json_get_bool(r, "save_thumbnail", p->save_thumbnail);
    p->android_auto_version = json_get_bool(r, "android_auto_version", p->android_auto_version);
    snprintf(p->android_icon_bg, sizeof p->android_icon_bg, "%s", json_get_str(r, "android_icon_bg", p->android_icon_bg));
    snprintf(p->android_privacy_url, sizeof p->android_privacy_url, "%s", json_get_str(r, "android_privacy_url", p->android_privacy_url));
    p->render_mode = json_get_int(r, "render_mode", p->render_mode);
    p->game_path_tracing = json_get_bool(r, "game_path_tracing", p->game_path_tracing);
    p->ray_tracing = json_get_bool(r, "ray_tracing", p->ray_tracing);
    p->static_lighting = json_get_bool(r, "static_lighting", p->static_lighting);
    p->auto_exposure = json_get_bool(r, "auto_exposure", p->auto_exposure);
    p->upscaler = json_get_int(r, "upscaler", p->upscaler);
    p->render_style = json_get_int(r, "render_style", p->render_style);
    p->stylized_bands = json_get_int(r, "stylized_bands", p->stylized_bands);
    p->stylized_softness = (float)json_get_num(r, "stylized_softness", p->stylized_softness);
    p->stylized_saturation = (float)json_get_num(r, "stylized_saturation", p->stylized_saturation);
    json_get_floats(r, "stylized_shadow_tint", p->stylized_shadow_tint, 3);
    p->stylized_outline = (float)json_get_num(r, "stylized_outline", p->stylized_outline);
    json_get_floats(r, "stylized_outline_color", p->stylized_outline_color, 3);
    p->stylized_outline_depth = (float)json_get_num(r, "stylized_outline_depth", p->stylized_outline_depth);
    p->stylized_anim_fps = json_get_int(r, "stylized_anim_fps", p->stylized_anim_fps);
    p->film_grain = (float)json_get_num(r, "film_grain", p->film_grain);
    p->shadow_denoise = json_get_int(r, "shadow_denoise", p->shadow_denoise);
    p->video_distortion = (float)json_get_num(r, "video_distortion", p->video_distortion);
    p->video_fringe = (float)json_get_num(r, "video_fringe", p->video_fringe);
    p->video_halation = (float)json_get_num(r, "video_halation", p->video_halation);
    p->video_vignette = (float)json_get_num(r, "video_vignette", p->video_vignette);
    p->image_denoise = json_get_int(r, "image_denoise", p->image_denoise);
    p->vrs = json_get_bool(r, "vrs", p->vrs);
    p->tsr_disocclusion = json_get_bool(r, "tsr_disocclusion", p->tsr_disocclusion); // (Q20)
    p->den_history_fix = json_get_bool(r, "den_history_fix", p->den_history_fix);
    p->async_compute = json_get_bool(r, "async_compute", p->async_compute);
    p->upscale_mode = json_get_int(r, "upscale_mode", p->upscale_mode);
    p->dyn_res = json_get_bool(r, "dynamic_resolution", p->dyn_res);
    p->dyn_res_fps = json_get_int(r, "dynamic_resolution_fps", p->dyn_res_fps);
    p->dyn_res_min = json_get_int(r, "dynamic_resolution_min", p->dyn_res_min);
    p->exposure_compensation = (float)json_get_num(r, "exposure_compensation", p->exposure_compensation);
    S("window_title", window_title);
    p->resolution_w = json_get_int(r, "resolution_w", p->resolution_w);
    p->resolution_h = json_get_int(r, "resolution_h", p->resolution_h);
    p->fullscreen = json_get_bool(r, "fullscreen", p->fullscreen);
    p->vsync = json_get_bool(r, "vsync", p->vsync);
    p->hdr_output = json_get_bool(r, "hdr_output", p->hdr_output);
    p->hdr_paper = (float)json_get_num(r, "hdr_paper", p->hdr_paper > 0.0f ? p->hdr_paper : 200.0);
    p->hdr_peak = (float)json_get_num(r, "hdr_peak", p->hdr_peak > 0.0f ? p->hdr_peak : 1000.0);
    p->mouse_capture = json_get_bool(r, "mouse_capture", p->mouse_capture);
    S("package_dir", package_dir);
    p->include_all_assets = json_get_bool(r, "include_all_assets", p->include_all_assets);
    S("game_name", game_name);
    S("icon", icon);
    S("copyright", copyright);
    p->shipping = json_get_bool(r, "shipping", p->shipping);
    p->package_platform = json_get_int(r, "package_platform", p->package_platform);
    S("splash", splash);
    p->show_intro = json_get_bool(r, "show_intro", p->show_intro);
    p->online_steam = json_get_bool(r, "online_steam", p->online_steam);
    p->steam_cloud = json_get_bool(r, "steam_cloud", p->steam_cloud);
    p->steam_app_id = (uint32_t)json_get_int(r, "steam_app_id", (int)p->steam_app_id);
    p->net_port = json_get_int(r, "net_port", p->net_port);
    p->net_max_players = json_get_int(r, "net_max_players", p->net_max_players);
    p->net_tick_rate = json_get_int(r, "net_tick_rate", p->net_tick_rate);
    p->net_budget_kbps = json_get_int(r, "net_budget_kbps", p->net_budget_kbps);
    p->net_interp_ms = (float)json_get_num(r, "net_interp_ms", p->net_interp_ms);
    p->net_relevancy_m = (float)json_get_num(r, "net_relevancy_m", p->net_relevancy_m);
    p->emu_latency_ms = (float)json_get_num(r, "emu_latency_ms", p->emu_latency_ms);
    p->emu_jitter_ms = (float)json_get_num(r, "emu_jitter_ms", p->emu_jitter_ms);
    p->emu_loss = (float)json_get_num(r, "emu_loss", p->emu_loss);
    json_free(&doc);
    free(t);
    if (p->max_substeps < 1) p->max_substeps = 1;
    if (p->max_substeps > 16) p->max_substeps = 16;
    if (p->solver_substeps < 1) p->solver_substeps = 1;
    if (p->solver_substeps > 8) p->solver_substeps = 8;
    if (p->physics_rate < 30) p->physics_rate = 30;
    if (p->physics_rate > 240) p->physics_rate = 240;
    snprintf(g_file, sizeof g_file, "%s", path);
    for (char* c = g_file; *c; c++)
        if (*c == '\\') *c = '/';
    return true;
}
#undef S

bool project_save(const char* path) {
    const char* to = path && path[0] ? path : g_file;
    if (!to[0]) return false;
    const ProjectSettings* p = project();
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version_file", 1);
    jw_kstr(&w, "engine", "Rivora Engine");
    jw_kstr(&w, "name", p->name), jw_kstr(&w, "company", p->company), jw_kstr(&w, "version", p->version);
    jw_kstr(&w, "description", p->description);
    jw_kstr(&w, "editor_startup_map", p->editor_startup_map), jw_kstr(&w, "game_default_map", p->game_default_map);
    jw_kstr(&w, "game_mode", p->game_mode), jw_kstr(&w, "default_pawn", p->default_pawn);
    jw_kstr(&w, "player_controller", p->player_controller), jw_kstr(&w, "hud", p->hud);
    jw_kstr(&w, "game_state", p->game_state), jw_kstr(&w, "player_state", p->player_state), jw_kstr(&w, "game_instance", p->game_instance);
    jw_kfloat(&w, "gravity_z", p->gravity_z), jw_kint(&w, "max_substeps", p->max_substeps), jw_kint(&w, "solver_substeps", p->solver_substeps), jw_kint(&w, "physics_rate", p->physics_rate);
    jw_kint(&w, "key_forward", p->key_forward), jw_kint(&w, "key_back", p->key_back), jw_kint(&w, "key_left", p->key_left);
    jw_kint(&w, "key_right", p->key_right), jw_kint(&w, "key_jump", p->key_jump);
    jw_kfloat(&w, "mouse_sensitivity", p->mouse_sensitivity), jw_kbool(&w, "invert_mouse_y", p->invert_mouse_y);
    jw_kstr(&w, "default_imc", p->default_imc);
    jw_kint(&w, "game_scalability", p->game_scalability), jw_kbool(&w, "ray_tracing", p->ray_tracing);
    jw_kint(&w, "audio_max_channels", p->audio_max_channels), jw_kbool(&w, "audio_binaural", p->audio_binaural);
    jw_kfloat(&w, "audio_master_volume", p->audio_master_volume);
    jw_kstr(&w, "android_package", p->android_package), jw_kstr(&w, "android_keystore", p->android_keystore);
    jw_kstr(&w, "android_key_alias", p->android_key_alias), jw_kstr(&w, "android_key_pass_file", p->android_key_pass_file);
    jw_kint(&w, "android_orientation", p->android_orientation), jw_kint(&w, "android_version_code", p->android_version_code);
    jw_kbool(&w, "android_touch_controls", p->android_touch_controls);
    jw_kbool(&w, "android_ar", p->android_ar);
    jw_kbool(&w, "android_ar_occlusion", p->android_ar_occlusion);
    jw_kint(&w, "android_quality", p->android_quality), jw_kint(&w, "android_target_fps", p->android_target_fps);
    jw_kbool(&w, "android_dynamic_res", p->android_dynamic_res), jw_kfloat(&w, "android_content_scale", p->android_content_scale);
    jw_kbool(&w, "android_auto_version", p->android_auto_version), jw_kstr(&w, "android_icon_bg", p->android_icon_bg);
    jw_kstr(&w, "android_privacy_url", p->android_privacy_url);
    { // (Q25) VR
        const ProjectVr* v = &p->vr;
        jw_kbool(&w, "vr_start", v->start_in_vr), jw_kbool(&w, "vr_pc", v->pc), jw_kbool(&w, "vr_quest", v->quest);
        jw_kint(&w, "vr_refresh_hz", v->refresh_hz), jw_kint(&w, "vr_tracking", v->tracking), jw_kfloat(&w, "vr_seated_height", v->seated_height);
        jw_kint(&w, "vr_locomotion", v->locomotion), jw_kint(&w, "vr_turn", v->turn), jw_kfloat(&w, "vr_snap_angle", v->snap_angle);
        jw_kfloat(&w, "vr_turn_speed", v->turn_speed), jw_kfloat(&w, "vr_move_speed", v->move_speed);
        jw_kfloat(&w, "vr_teleport_range", v->teleport_range), jw_kfloat(&w, "vr_vignette", v->vignette);
        jw_kbool(&w, "vr_hands", v->hands), jw_kbool(&w, "vr_hand_tracking", v->hand_tracking), jw_kbool(&w, "vr_laser", v->laser), jw_kbool(&w, "vr_drag", v->drag_locomotion);
        jw_kbool(&w, "vr_physical_hands", v->physical_hands), jw_kbool(&w, "vr_hand_push", v->hand_push), jw_kbool(&w, "vr_hand_objects", v->hand_objects), jw_kbool(&w, "vr_held_collide", v->held_collide);
        jw_kbool(&w, "vr_hide_body", v->hide_body), jw_kint(&w, "vr_head_collision", v->head_collision);
        jw_kstr(&w, "vr_hand_mesh_l", v->hand_mesh[0]), jw_kstr(&w, "vr_hand_mesh_r", v->hand_mesh[1]), jw_kfloat(&w, "vr_hand_mesh_scale", v->hand_mesh_scale);
        jw_kfloats(&w, "vr_hand_mesh_rot", v->hand_mesh_rot, 3);
        jw_kint(&w, "vr_dominant_hand", v->dominant_hand), jw_kint(&w, "vr_move_direction", v->move_direction);
        jw_kfloat(&w, "vr_teleport_fade", v->teleport_fade), jw_kbool(&w, "vr_turn_fade", v->turn_fade);
        jw_kfloat(&w, "vr_grab_reach", v->grab_reach), jw_kbool(&w, "vr_distance_grab", v->distance_grab);
        jw_kfloat(&w, "vr_throw_scale", v->throw_scale), jw_kfloat(&w, "vr_haptics", v->haptics), jw_kfloat(&w, "vr_laser_length", v->laser_length);
        jw_kfloat(&w, "vr_menu_distance", v->menu_distance), jw_kfloat(&w, "vr_menu_width", v->menu_width), jw_kint(&w, "vr_msaa", v->msaa), jw_kint(&w, "vr_headroom_fps", v->headroom_fps), jw_kbool(&w, "vr_safety_net", v->safety_net);
        jw_kint(&w, "vr_mirror_mode", v->mirror_mode), jw_kfloat(&w, "vr_spectator_fov", v->spectator_fov), jw_kfloat(&w, "vr_spectator_smooth", v->spectator_smooth);
        jw_kint(&w, "vr_grab_gesture", v->grab_gesture), jw_kint(&w, "vr_drag_gesture", v->drag_gesture);
        jw_kint(&w, "vr_quality", v->quality), jw_kint(&w, "vr_foveation", v->foveation);
        jw_kbool(&w, "vr_foveation_dynamic", v->foveation_dynamic), jw_kbool(&w, "vr_spacewarp", v->spacewarp);
        jw_kfloat(&w, "vr_render_scale", v->render_scale);
        jw_kbool(&w, "mr_enabled", v->mr_enabled), jw_kbool(&w, "mr_passthrough", v->mr_passthrough), jw_kint(&w, "mr_background", v->mr_background); // (Q25b)
        jw_kfloat(&w, "mr_opacity", v->mr_opacity), jw_kint(&w, "mr_style", v->mr_style), jw_kfloat(&w, "mr_brightness", v->mr_brightness);
        jw_kfloat(&w, "mr_contrast", v->mr_contrast), jw_kfloat(&w, "mr_saturation", v->mr_saturation);
        jw_kfloats(&w, "mr_tint", v->mr_tint, 3), jw_kfloats(&w, "mr_edge", v->mr_edge, 4), jw_kfloat(&w, "mr_virtual_opacity", v->mr_virtual_opacity);
        jw_kbool(&w, "mr_scene", v->mr_scene), jw_kbool(&w, "mr_scene_actors", v->mr_scene_actors), jw_kbool(&w, "mr_room_mesh", v->mr_room_mesh);
        jw_kbool(&w, "mr_ask_scan", v->mr_ask_scan), jw_kbool(&w, "mr_align", v->mr_align), jw_kint(&w, "mr_occlusion", v->mr_occlusion);
        jw_kfloat(&w, "mr_occlusion_soft", v->mr_occlusion_soft), jw_kbool(&w, "mr_hand_removal", v->mr_hand_removal), jw_kfloat(&w, "mr_shadows", v->mr_shadows);
        jw_kint(&w, "mr_light", v->mr_light), jw_kfloat(&w, "mr_ev100", v->mr_ev100), jw_kbool(&w, "mr_windows_real", v->mr_windows_real);
        jw_kbool(&w, "mr_hide_floor", v->mr_hide_floor), jw_kbool(&w, "mr_depth_flip", v->mr_depth_flip), jw_kbool(&w, "mr_unpremultiplied", v->mr_unpremultiplied);
    }
    jw_kbool(&w, "pause_menu", p->pause_menu), jw_kstr(&w, "pause_menu_widget", p->pause_menu_widget);
    jw_kstr(&w, "pie_pause_key", p->pie_pause_key), jw_kstr(&w, "main_menu_map", p->main_menu_map);
    jw_kstr(&w, "save_game_class", p->save_game_class), jw_kfloat(&w, "autosave_interval", p->autosave_interval);
    jw_kint(&w, "autosave_slots", p->autosave_slots), jw_kbool(&w, "autosave_on_level", p->autosave_on_level);
    jw_kbool(&w, "save_thumbnail", p->save_thumbnail);
    jw_kint(&w, "render_mode", p->render_mode), jw_kbool(&w, "game_path_tracing", p->game_path_tracing);
    jw_kbool(&w, "static_lighting", p->static_lighting), jw_kbool(&w, "auto_exposure", p->auto_exposure);
    jw_kint(&w, "upscaler", p->upscaler), jw_kint(&w, "upscale_mode", p->upscale_mode), jw_kint(&w, "render_style", p->render_style);
    jw_kint(&w, "stylized_bands", p->stylized_bands), jw_kfloat(&w, "stylized_softness", p->stylized_softness);
    jw_kfloat(&w, "stylized_saturation", p->stylized_saturation), jw_kfloats(&w, "stylized_shadow_tint", p->stylized_shadow_tint, 3);
    jw_kfloat(&w, "stylized_outline", p->stylized_outline), jw_kfloats(&w, "stylized_outline_color", p->stylized_outline_color, 3);
    jw_kfloat(&w, "stylized_outline_depth", p->stylized_outline_depth), jw_kint(&w, "stylized_anim_fps", p->stylized_anim_fps);
    jw_kfloat(&w, "film_grain", p->film_grain), jw_kint(&w, "image_denoise", p->image_denoise);
    jw_kint(&w, "shadow_denoise", p->shadow_denoise);
    jw_kfloat(&w, "video_distortion", p->video_distortion), jw_kfloat(&w, "video_fringe", p->video_fringe);
    jw_kfloat(&w, "video_halation", p->video_halation), jw_kfloat(&w, "video_vignette", p->video_vignette);
    jw_kbool(&w, "vrs", p->vrs);
    jw_kbool(&w, "tsr_disocclusion", p->tsr_disocclusion), jw_kbool(&w, "den_history_fix", p->den_history_fix); // (Q20)
    jw_kbool(&w, "async_compute", p->async_compute);
    jw_kbool(&w, "dynamic_resolution", p->dyn_res), jw_kint(&w, "dynamic_resolution_fps", p->dyn_res_fps), jw_kint(&w, "dynamic_resolution_min", p->dyn_res_min);
    jw_kfloat(&w, "exposure_compensation", p->exposure_compensation);
    jw_kstr(&w, "window_title", p->window_title), jw_kint(&w, "resolution_w", p->resolution_w), jw_kint(&w, "resolution_h", p->resolution_h);
    jw_kbool(&w, "fullscreen", p->fullscreen), jw_kbool(&w, "vsync", p->vsync), jw_kbool(&w, "mouse_capture", p->mouse_capture);
    jw_kbool(&w, "hdr_output", p->hdr_output), jw_kfloat(&w, "hdr_paper", p->hdr_paper), jw_kfloat(&w, "hdr_peak", p->hdr_peak);
    jw_kstr(&w, "package_dir", p->package_dir), jw_kbool(&w, "include_all_assets", p->include_all_assets);
    jw_kstr(&w, "game_name", p->game_name), jw_kstr(&w, "icon", p->icon), jw_kstr(&w, "copyright", p->copyright);
    jw_kbool(&w, "shipping", p->shipping);
    jw_kint(&w, "package_platform", p->package_platform);
    jw_kstr(&w, "splash", p->splash), jw_kbool(&w, "show_intro", p->show_intro);
    jw_kbool(&w, "online_steam", p->online_steam), jw_kbool(&w, "steam_cloud", p->steam_cloud), jw_kint(&w, "steam_app_id", (int)p->steam_app_id);
    jw_kint(&w, "net_port", p->net_port), jw_kint(&w, "net_max_players", p->net_max_players), jw_kint(&w, "net_tick_rate", p->net_tick_rate);
    jw_kint(&w, "net_budget_kbps", p->net_budget_kbps), jw_kfloat(&w, "net_interp_ms", p->net_interp_ms), jw_kfloat(&w, "net_relevancy_m", p->net_relevancy_m);
    jw_kfloat(&w, "emu_latency_ms", p->emu_latency_ms), jw_kfloat(&w, "emu_jitter_ms", p->emu_jitter_ms), jw_kfloat(&w, "emu_loss", p->emu_loss);
    jw_obj_end(&w);
    bool ok = false;
    if (!w.fail) {
        char tmp[700];
        snprintf(tmp, sizeof tmp, "%s.tmp", to);
        FILE* f = re_fopen(tmp, "wb");
        if (f) {
            ok = fwrite(w.buf, 1, w.len, f) == w.len;
            ok = fclose(f) == 0 && ok && re_replace_file(tmp, to);
        }
    }
    jw_free(&w);
    if (ok && to != g_file) snprintf(g_file, sizeof g_file, "%s", to);
    return ok;
}

// --- Recent projects ---

static bool recent_path(char* out, size_t cap) {
    char dir[512];
    if (!re_config_dir(dir, sizeof dir)) return false;
    snprintf(out, cap, "%s\\projects.txt", dir);
    return true;
}

int project_recent(char (*out)[600], int cap) {
    char path[600], line[700];
    if (!recent_path(path, sizeof path)) return 0;
    FILE* f = re_fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        char* e = strpbrk(line, "\r\n");
        if (e) *e = 0;
        if (line[0] && re_file_stat(line, NULL, NULL, NULL)) snprintf(out[n++], 600, "%s", line);
    }
    fclose(f);
    return n;
}

static void recent_write(char (*list)[600], int n) {
    char path[600];
    if (!recent_path(path, sizeof path)) return;
    FILE* f = re_fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", list[i]);
    fclose(f);
}

static bool g_no_recent;
void project_recent_disable(void) { g_no_recent = true; }

void project_recent_add(const char* p) {
    if (g_no_recent) return;
    static char list[32][600];
    int n = project_recent(list, 32), k = 0;
    static char out[32][600];
    snprintf(out[k++], 600, "%s", p);
    for (int i = 0; i < n && k < 32; i++)
        if (_stricmp(list[i], p)) snprintf(out[k++], 600, "%s", list[i]);
    recent_write(out, k);
}

void project_recent_remove(const char* p) {
    static char list[32][600];
    int n = project_recent(list, 32), k = 0;
    static char out[32][600];
    for (int i = 0; i < n; i++)
        if (_stricmp(list[i], p)) snprintf(out[k++], 600, "%s", list[i]);
    recent_write(out, k);
}

// --- Play: the Game Mode's classes ---

static Entity spawn_named(World* w, const char* cls_name, const float at[3], const float rot[3]) {
    if (!cls_name[0]) return ENTITY_NONE;
    BpAsset* c = bp_find_name(BPA_CLASS, cls_name);
    return c ? bp_spawn(w, c, at, rot) : ENTITY_NONE;
}

static void begin_play_classes(World* w);
void project_begin_play(World* w) {
    begin_play_classes(w);
    demo_play_begin(w, project()->game_mode); // a demo's native game mode (its pawn possessed by now; demo_play.c)
    // the touch controls (Project Settings > Android): on a touch screen, or REALENGINE_TOUCH=1 (the PC: the mouse is a finger)
    bool touch = false;
#ifdef __ANDROID__
    touch = true;
#endif
    const char* env = getenv("REALENGINE_TOUCH");
    if (env && env[0]) touch = env[0] == '1';
    if (touch && project()->android_touch_controls) {
        BpAsset* a = bp_find_hash(ui_touch_controls_asset());
        if (a) {
            Entity e = ui_create_widget(w, a);
            if (e != ENTITY_NONE) ui_add_to_viewport(e, 100);
        }
    }
}

static void begin_play_classes(World* w) {
    const ProjectSettings* p = project();
    const float zero[3] = { 0, 0, 0 };
    // the "manager" classes (no place in the world: their graphs run)
    spawn_named(w, p->game_instance, zero, zero);
    spawn_named(w, p->game_mode, zero, zero);
    spawn_named(w, p->game_state, zero, zero);
    spawn_named(w, p->player_controller, zero, zero);
    spawn_named(w, p->player_state, zero, zero);
    spawn_named(w, p->hud, zero, zero);
    // the Default Pawn at the Player Start, when the level gave none to the player
    if (!p->default_pawn[0] || bp_player(w) != ENTITY_NONE) return;
    float at[3] = { 0, 0, 100 }, rot[3] = { 0, 0, 0 };
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_PLAYER_START) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        memcpy(at, t[0].location, 12), rot[1] = t[0].rotation[1];
        break;
    }
    spawn_named(w, p->default_pawn, at, rot);
}

bool project_save_copy(const char* path) { // the settings at another path, the open project's file kept
    char keep[600];
    snprintf(keep, sizeof keep, "%s", g_file);
    bool ok = project_save(path);
    snprintf(g_file, sizeof g_file, "%s", keep);
    return ok;
}

void project_apply_audio(void) {
    const ProjectSettings* p = project();
    AuProject a;
    memset(&a, 0, sizeof a);
    a.max_channels = (uint32_t)(p->audio_max_channels > 0 ? p->audio_max_channels : 64);
    a.binaural = p->audio_binaural;
    a.master_volume = p->audio_master_volume > 0.0f ? p->audio_master_volume : 1.0f;
    audio_set_project(&a);
}
