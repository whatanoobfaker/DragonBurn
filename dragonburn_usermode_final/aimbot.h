#pragma once
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "aimbot_math.h"
#include "entity_reader.h"
#include "overlay.h"
#include "settings.h"
#include "shared_state.h"
#include "types.h"
#include "utils.h"
#include "input/input.h"
#include "systems/bvh.h"

static std::thread g_aimbot_thread;
static std::atomic<bool> g_aimbot_running{ false };

static float aim_error_x = 0.0f;
static float aim_error_y = 0.0f;
static float rcs_error_x = 0.0f;
static float rcs_error_y = 0.0f;
static Vec3  rcs_old_punch{};
static bool  rcs_punch_init = false;

static bool is_gun_weapon(uint16_t def) {
    if (def == 0 || def == 31 || def == 42 || def == 49 || def == 59) return false;
    if (def >= 43 && def <= 48) return false;
    if (def >= 500) return false;
    return true;
}

static AimAngles view_with_punch_removed(const AimbotFrame& frame) {
    AimAngles a{};
    a.pitch = frame.view_angles.x;
    a.yaw   = frame.view_angles.y;
    if (g_settings.aimbot_rcs_compensate && frame.shots_fired > 0) {
        const float s = g_settings.rcs_strength;
        a.pitch -= frame.aim_punch.x * 2.0f * s;
        a.yaw   -= frame.aim_punch.y * 2.0f * s;
        a.yaw = normalize_yaw(a.yaw);
    }
    return a;
}

static void rcs_tick() {
    if (!g_settings.master_switch || !g_settings.rcs_enabled)
        return;

    AimbotFrame frame = g_aimbot_data.snapshot();
    if (frame.local_pawn == 0 || frame.screen_w == 0 || !frame.camera_valid)
        return;
    if (!is_gun_weapon(frame.local_weapon_def_index)) {
        rcs_punch_init = false;
        rcs_error_x = rcs_error_y = 0.f;
        return;
    }

    // Aimbot already holds the key — still apply punch delta when shooting
    const bool aiming = g_settings.aimbot_enabled &&
                        (GetAsyncKeyState(g_settings.key_aimbot) & 0x8000);
    // Avoid double-compensation: aimbot already aims through punch
    if (aiming && g_settings.aimbot_rcs_compensate) {
        rcs_old_punch = frame.aim_punch;
        rcs_punch_init = true;
        return;
    }
    if (!g_settings.rcs_standalone && !aiming) {
        rcs_punch_init = false;
        return;
    }

    if (frame.shots_fired < g_settings.rcs_start_bullet) {
        rcs_old_punch = frame.aim_punch;
        rcs_punch_init = true;
        rcs_error_x = rcs_error_y = 0.f;
        return;
    }

    if (!rcs_punch_init) {
        rcs_old_punch = frame.aim_punch;
        rcs_punch_init = true;
        return;
    }

    const float strength = 2.0f * g_settings.rcs_strength;
    float dp = (frame.aim_punch.x - rcs_old_punch.x) * strength;
    float dy = (frame.aim_punch.y - rcs_old_punch.y) * strength;
    rcs_old_punch = frame.aim_punch;

    if (fabsf(dp) < 1e-5f && fabsf(dy) < 1e-5f)
        return;

    float deg_per_pixel = frame.camera_fov / static_cast<float>(frame.screen_w);
    if (deg_per_pixel < 1e-6f) return;

    // Compensate punch: counter-yaw left/right, counter-pitch up/down
    float move_x = -dy / deg_per_pixel;
    float move_y =  dp / deg_per_pixel;

    rcs_error_x += move_x;
    rcs_error_y += move_y;
    int dx = static_cast<int>(rcs_error_x);
    int dyi = static_cast<int>(rcs_error_y);
    rcs_error_x -= static_cast<float>(dx);
    rcs_error_y -= static_cast<float>(dyi);

    if (dx != 0 || dyi != 0)
        g_input.inject_mouse(dx, dyi, Input::move);
}

static bool  g_trigger_waiting      = false;
static bool  g_trigger_held         = false;
static float g_trigger_delay_end    = 0.0f;
static float g_trigger_release_time = 0.0f;

struct TriggerbotResult {
    bool found = false;
    float distance_sq = FLT_MAX; // Use squared distance for comparison
    Vec3  hit_pos{};
    // We omit player*, bones, hitbox, damage, penetrated as per request
};

static TriggerbotResult triggerbot_trace_crosshair(
    const Vec3& eye_pos,
    const AimAngles& view_angles,
    const AimbotFrame& frame)
{
    TriggerbotResult result{};

    float pitch_rad = view_angles.pitch * DEG2RAD;
    float yaw_rad   = view_angles.yaw   * DEG2RAD;

    Vec3 forward;
    forward.x = cosf(pitch_rad) * cosf(yaw_rad);
    forward.y = cosf(pitch_rad) * sinf(yaw_rad);
    forward.z = -sinf(pitch_rad);
    forward = forward.normalized();

    constexpr auto MAX_RANGE = 8192.0f;

    struct BoneInfo {
        Vec3 AimbotFrame::Target::* bone_ptr;
        float radius;
    };

    BoneInfo hitboxes_to_check[] = {
        { &AimbotFrame::Target::head_pos,   3.5f },
        { &AimbotFrame::Target::neck_pos,   3.0f },
        { &AimbotFrame::Target::chest_pos,  5.0f },
        { &AimbotFrame::Target::pelvis_pos, 4.0f },
    };
    int num_hitboxes = 4;

    bool bvh_valid = g_bvh.valid();

    for (int i = 1; i < EntityList::MAX_PLAYERS; i++)
    {
        const auto& t = frame.targets[i];
        if (!t.valid || t.health <= 0) continue;
        if (t.team == frame.local_team) continue;

        if (frame.local_player_index >= 0) {
            if (!(t.bSpottedByMask & (uint64_t(1) << frame.local_player_index)))
                continue;
        }

        for (int hb_idx = 0; hb_idx < num_hitboxes; hb_idx++)
        {
            const auto& current_hb_info = hitboxes_to_check[hb_idx];
            Vec3 bone_center = t.*(current_hb_info.bone_ptr);
            float radius = current_hb_info.radius;

            Vec3 oc = eye_pos - bone_center;
            float a = forward.dot(forward);
            float b = 2.0f * oc.dot(forward);
            float c = oc.dot(oc) - radius * radius;
            float discriminant = b * b - 4.0f * a * c;

            if (discriminant < 0.0f) continue;

            float sqrt_d = std::sqrtf(discriminant);
            float t_val = (-b - sqrt_d) / (2.0f * a);

            if (t_val < 0.0f)
            {
                t_val = (-b + sqrt_d) / (2.0f * a);
            }

            if (t_val < 0.0f || t_val > MAX_RANGE) continue;

            Vec3 hit_pos = eye_pos + forward * t_val;
            float dist_sq = (hit_pos - eye_pos).length_sqr();

            if (dist_sq >= result.distance_sq) continue;

            bool visible = true;
            if (bvh_valid)
            {
                const auto vis_trace = g_bvh.trace_ray(eye_pos, bone_center);
                visible = !vis_trace.hit || vis_trace.fraction > 0.97f;
            }

            if (visible)
            {
                result.distance_sq = dist_sq;
                result.hit_pos = hit_pos;
                result.found = true;
            }
            // Skipping autowall logic as per request
        }
    }
    return result;
}

static float get_time_seconds()
{
    static auto start = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<float>(now - start).count();
}

static void triggerbot_tick()
{
    if (!g_settings.master_switch || !g_settings.triggerbot_enabled)
    {
        g_trigger_waiting = false;
        g_trigger_held = false; // Ensure mouse released if disabled mid-shot
        return;
    }

    float now = get_time_seconds();

    // ─── Handle held mouse button release ─────────────
    if (g_trigger_held)
    {
        if (now >= g_trigger_release_time)
        {
            g_input.inject_mouse(0, 0, Input::left_up);
            g_trigger_held = false;
        }
        return; // Don't try to trace/shoot again while button is held down
    }

    // ─── Get frame data ───────────────────────────────
    AimbotFrame frame = g_aimbot_data.snapshot();

    if (frame.local_pawn == 0 || frame.screen_w == 0) return;
    if (!frame.camera_valid) return;

    // ─── Check trigger key (or always-on mode) ────────
    if (!(GetAsyncKeyState(g_settings.key_triggerbot) & 0x8000) && !g_settings.triggerbot_always_on)
    {
        g_trigger_waiting = false;
        return;
    }

    // ─── Scoped-only check ────────────────────────────
    if (g_settings.triggerbot_scoped_only && !frame.is_scoped)
    {
        auto w = frame.local_weapon_def_index;
        if (w == 9 || w == 40)
        {
            g_trigger_waiting = false;
            return;
        }
    }

    Vec3 eye_pos = frame.eye_origin;
    AimAngles view_angles;
    view_angles.pitch = frame.view_angles.x;
    view_angles.yaw   = frame.view_angles.y;

    TriggerbotResult trace_result = triggerbot_trace_crosshair(
        eye_pos, view_angles, frame);

    // ─── Not on enemy — reset delay ───────────────────
    if (!trace_result.found)
    {
        g_trigger_waiting = false;
        return;
    }

    // ─── Fire immediately when delay is 0 ─────────────
    if (g_settings.triggerbot_delay == 0)
    {
        g_trigger_waiting = false;
        g_trigger_held = false;
        g_input.inject_mouse(0, 0, Input::left_down);
        g_input.inject_mouse(0, 0, Input::left_up);
        return;
    }

    // ─── Delay logic (non-zero delay) ─────────────────
    if (!g_trigger_waiting)
    {
        g_trigger_waiting = true;
        g_trigger_delay_end = now
            + static_cast<float>(g_settings.triggerbot_delay) * 0.001f;
        return;
    }

    if (now < g_trigger_delay_end)
        return;

    // ─── Fire! ────────────────────────────────────────
    g_trigger_waiting = false;
    g_input.inject_mouse(0, 0, Input::left_down);
    g_trigger_held = true;

    float hold_ms = 50.0f + static_cast<float>(rand() % 70);
    g_trigger_release_time = now + hold_ms * 0.001f;
}

static void aimbot_tick() {
    if (!g_settings.master_switch || !g_settings.aimbot_enabled) return;
    if (!(GetAsyncKeyState(g_settings.key_aimbot) & 0x8000)) return;

    AimbotFrame frame = g_aimbot_data.snapshot();

    if (frame.local_pawn == 0 || frame.screen_w == 0) return;
    if (!frame.camera_valid) return;

    Vec3 eye_pos = frame.eye_origin;
    AimAngles view_angles = view_with_punch_removed(frame);

    float deg_per_pixel = frame.camera_fov
                        / static_cast<float>(frame.screen_w);

    static const Vec3 AimbotFrame::Target::* bone_list[] = {
        &AimbotFrame::Target::head_pos,
        &AimbotFrame::Target::neck_pos,
        &AimbotFrame::Target::chest_pos,
        &AimbotFrame::Target::pelvis_pos,
    };

    const int primary = g_settings.aimbot_bone;
    const bool multipoint = g_settings.aimbot_multipoint;
    const auto fov_limit = static_cast<float>(g_settings.aimbot_fov);
    const bool do_vis = g_settings.aimbot_visible_check;
    const bool bvh_valid = do_vis && g_bvh.valid();

    float best_fov = fov_limit;
    bool  found = false;
    Vec3  best_aim_point{};

    for (int i = 1; i < EntityList::MAX_PLAYERS; i++)
    {
        if (!g_aimbot_running.load(std::memory_order_relaxed))
            return;

        const auto& t = frame.targets[i];
        if (!t.valid || t.health <= 0)  continue;
        if (t.team == frame.local_team) continue;

        if (do_vis && frame.local_player_index >= 0) {
            if (!(t.bSpottedByMask & (uint64_t(1) << frame.local_player_index)))
                continue;
        }

        // Preferred bone first, then nearby multipoints
        int order[4];
        int order_n = 0;
        order[order_n++] = primary;
        if (multipoint) {
            for (int b = 0; b < 4; b++) {
                if (b == primary) continue;
                order[order_n++] = b;
            }
        }

        for (int oi = 0; oi < order_n; oi++)
        {
            Vec3 bone_pos = t.*(bone_list[order[oi]]);

            if (g_settings.aimbot_prediction) {
                bone_pos = bone_pos + t.velocity * g_settings.aimbot_predict_time;
            }

            AimAngles desired = calculate_angle(eye_pos, bone_pos);
            float fov = get_fov_between(view_angles, desired);

            if (fov > best_fov) continue;

            if (bvh_valid)
            {
                auto trace = g_bvh.trace_ray(eye_pos, bone_pos);
                if (trace.hit && trace.fraction <= 0.97f)
                    continue;
            }

            best_fov       = fov;
            best_aim_point = bone_pos;
            found          = true;

            if (!multipoint)
                break;
            // With multipoint, keep scanning for closer FOV on this player
        }
    }

    if (found)
    {
        AimAngles desired = calculate_angle(eye_pos, best_aim_point);

        float delta_pitch = desired.pitch - view_angles.pitch;
        float delta_yaw   = normalize_yaw(desired.yaw - view_angles.yaw);

        if (g_settings.aimbot_humanize) {
            const float h = g_settings.aimbot_humanize_strength;
            delta_pitch += ((static_cast<float>(rand() % 2001) / 1000.f) - 1.f) * h * 0.15f;
            delta_yaw   += ((static_cast<float>(rand() % 2001) / 1000.f) - 1.f) * h * 0.15f;
        }

        if (g_settings.aimbot_smooth > 1.0f)
        {
            delta_pitch /= g_settings.aimbot_smooth;
            delta_yaw   /= g_settings.aimbot_smooth;
        }

        float move_x = -delta_yaw   / deg_per_pixel;
        float move_y =  delta_pitch / deg_per_pixel;

        aim_error_x += move_x;
        aim_error_y += move_y;

        int dx = static_cast<int>(aim_error_x);
        int dy = static_cast<int>(aim_error_y);

        aim_error_x -= static_cast<float>(dx);
        aim_error_y -= static_cast<float>(dy);

        if (dx != 0 || dy != 0)
            g_input.inject_mouse(dx, dy, Input::move);
    }
    else
    {
        aim_error_x = aim_error_y = 0.0f;
    }
}

static void draw_aimbot_fov(ImDrawList* draw, int sw, int sh, float camera_fov)
{
    if (!draw || !g_settings.master_switch || !g_settings.aimbot_enabled ||
        !g_settings.aimbot_draw_fov)
        return;
    if (camera_fov < 1.f) camera_fov = 90.f;

    const float cx = sw * 0.5f;
    const float cy = sh * 0.5f;
    // Approximate FOV circle in screen space
    float radius = (g_settings.aimbot_fov / camera_fov) * (sw * 0.5f);
    if (radius < 4.f) radius = 4.f;
    if (radius > sw) radius = static_cast<float>(sw);

    draw->AddCircle({ cx, cy }, radius, IM_COL32(255, 255, 255, 70), 64, 1.25f);
}

static void aimbot_thread_func()
{
    printf("[+] Aimbot thread started (tid: %lu)\n", GetCurrentThreadId());

    static double tick_fps = 128.0f;

    while (g_aimbot_running.load(std::memory_order_relaxed))
    {
        auto tick_start = std::chrono::high_resolution_clock::now();

        if (g_overlay.is_game_window()) {
            triggerbot_tick();
            rcs_tick();
            aimbot_tick();
        }

        limit_frame(tick_start, tick_fps);
    }

    if (g_trigger_held)
    {
        g_input.inject_mouse(0, 0, Input::left_up);
        g_trigger_held = false;
    }

    printf("[+] Aimbot thread exiting cleanly\n");
}

static void start_aimbot_thread()
{
    if (g_aimbot_running.load())
        return;

    g_aimbot_running.store(true);
    g_aimbot_thread = std::thread(aimbot_thread_func);
}

static void stop_aimbot_thread()
{
    if (!g_aimbot_running.load())
        return;

    g_aimbot_running.store(false);

    if (g_aimbot_thread.joinable())
    {
        g_aimbot_thread.join();
    }
}