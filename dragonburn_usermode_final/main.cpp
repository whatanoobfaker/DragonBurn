#include <Windows.h>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <imgui.h>

#include "aimbot.h"
#include "types.h"
#include "settings.h"
#include "utils.h"
#include "offsets.h"
#include "memory/imemory.h"
#include "memory/memory_winapi.h"
#include "memory/memory_syscall.h"
#include "memory/memory_driver.h"
#include "memory/driver_manager.h"
#include "config.h"
#include "UIAccess.h"
#include "menu.h"
#include "crosshair.h"
#include "overlay.h"
#include "weapon_icons.h"
#include "entity_reader.h"
#include "visible_esp.h"
#include "spectators.h"
#include "radar.h"
#include "grenades.h"
#include "c4_esp.h"
#include "world_esp.h"
#include "input/input.h"

static std::string CONFIG_PATH;
static volatile bool g_running = true;
static bool g_driver_backend_active = false;

static void save_and_exit() {
    // Always persist — focus should not discard user settings
    if (Config::save(CONFIG_PATH))
        printf("[+] Config saved\n");
    else
        printf("[!] Config save failed\n");
}

// Cleanup that ALWAYS runs, no matter how we exit
static void cleanup_on_exit() {
    stop_aimbot_thread();
    if (g_memory) {
        g_memory->close();
        g_memory.reset();
    }
}

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_BREAK_EVENT || event == CTRL_LOGOFF_EVENT ||
        event == CTRL_SHUTDOWN_EVENT)
    {
        g_aimbot_running.store(false, std::memory_order_relaxed);
        g_running = false;

        save_and_exit();
        cleanup_on_exit();
        return TRUE;
    }
    return FALSE;
}

// Crash handler to ensure driver cleanup even on crash
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ex) {
    UNREFERENCED_PARAMETER(ex);
    printf("\n[!] Crash detected, cleaning up driver...\n");

    if (g_driver_backend_active) {
        DriverManager::full_cleanup(false);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

enum MemoryBackend {
    WinApi = 1,
    IndirectSyscall = 2,
    KernelDriver = 3
};

static const char* backend_name(int backend) {
    switch (backend) {
        case WinApi: return "WinAPI";
        case IndirectSyscall: return "Indirect Syscall";
        case KernelDriver: return "Kernel Driver (kdmapper)";
        default: return "Unknown";
    }
}

std::unique_ptr<IMemory> CreateMemoryBackend(MemoryBackend backend) {
    switch (backend) {
        case WinApi:           return std::make_unique<MemoryWinApi>();
        case IndirectSyscall:  return std::make_unique<MemorySyscall>();
        case KernelDriver:     return std::make_unique<MemoryDriver>(true);
        default:               throw std::runtime_error("invalid backend");
    }
}

int main() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    char* last_slash = strrchr(exe_path, '\\');
    if (last_slash) {
        last_slash[1] = 0;
        CONFIG_PATH = std::string(exe_path) + "cs2esp.ini";
    } else {
        CONFIG_PATH = "cs2esp.ini";
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();

    PrepareForUIAccess();

    SetConsoleCtrlHandler(console_handler, TRUE);
    SetUnhandledExceptionFilter(crash_handler);
    std::atexit([]() {
        stop_aimbot_thread();
        if (g_driver_backend_active) {
            DriverManager::restore_host_state();
        }
    });

    if (Config::load(CONFIG_PATH))
        printf("[+] Config loaded\n");

    if (g_settings.memory_backend < WinApi || g_settings.memory_backend > KernelDriver)
        g_settings.memory_backend = KernelDriver;

    g_grenades.init("grenades.json");

    const auto backend = static_cast<MemoryBackend>(g_settings.memory_backend);
    printf("[*] Memory backend: %s\n", backend_name(backend));
    g_driver_backend_active = (backend == KernelDriver);

    try {
        g_memory = CreateMemoryBackend(backend);
    }
    catch (const std::exception& e) {
        printf("[-] Failed to create backend: %s\n", e.what());
        CoUninitialize();
        system("pause");
        return 1;
    }

    // Wait for CS2 process (bounded)
    constexpr int kAttachTimeoutSec = 300;
    printf("[*] Waiting for CS2 (timeout %ds)...\n", kAttachTimeoutSec);
    bool attached = false;
    for (int i = 0; i < kAttachTimeoutSec; i++) {
        if (g_memory->attach(L"cs2.exe")) {
            attached = true;
            break;
        }
        if (i % 5 == 0)
            printf(".");
        Sleep(1000);
    }
    printf("\n");
    if (!attached) {
        printf("[-] Timed out waiting for cs2.exe\n");
        cleanup_on_exit();
        CoUninitialize();
        system("pause");
        return 1;
    }

    printf("[+] Attached to cs2.exe (PID: %lu)\n", g_memory->get_pid());
    printf("[+] client.dll base: 0x%llX\n",
        (unsigned long long)g_memory->get_client_base());

    // Retry module init up to 30 times (CS2 may still be loading DLLs)
    printf("[*] Initializing addresses");
    bool inited = false;
    for (int i = 0; i < 30; i++) {
        printf(".");
        if (g_memory->get_client_base() != 0) {
            inited = true;
            break;
        }
        // Re-query modules in case DLLs finished loading
        g_memory->attach(L"cs2.exe");
        Sleep(1000);
    }
    printf("\n");
    if (!inited) {
        printf("[-] Failed to init addresses after 30s (client.dll base is 0)\n");
        cleanup_on_exit();
        CoUninitialize();
        system("pause");
        return 1;
    }

    // Prefer offsets next to the executable
    std::string offsets_json = std::string(exe_path) + "offsets\\offsets.json";
    std::string client_json  = std::string(exe_path) + "offsets\\client_dll.json";
    if (!std::filesystem::exists(offsets_json)) {
        offsets_json = "offsets/offsets.json";
        client_json  = "offsets/client_dll.json";
    }

    if (!g_offsets.load(offsets_json, client_json)) {
        printf("[-] Failed to load offsets\n");
        cleanup_on_exit();
        CoUninitialize();
        system("pause");
        return 1;
    }

    // Wait for CS2 window (bounded)
    constexpr int kWindowTimeoutSec = 120;
    printf("[*] Waiting for CS2 window (timeout %ds)...\n", kWindowTimeoutSec);
    HWND cs2_hwnd = nullptr;
    for (int i = 0; i < kWindowTimeoutSec * 2; i++) {
        cs2_hwnd = FindWindowW(L"SDL_app", L"Counter-Strike 2");
        if (cs2_hwnd)
            break;
        Sleep(500);
    }
    if (!cs2_hwnd) {
        printf("[-] Timed out waiting for CS2 window\n");
        cleanup_on_exit();
        CoUninitialize();
        system("pause");
        return 1;
    }
    printf("[+] CS2 window found\n");
    if (!g_overlay.init(L"Counter-Strike 2")) {
        printf("[-] Overlay failed\n");
        cleanup_on_exit();
        CoUninitialize();
        system("pause");
        return 1;
    }

    if (!g_input.initialize())
    {
        printf("[-] Input init failed\n");
        cleanup_on_exit();
        CoUninitialize();
        system("pause");
        return 1;
    }

    if (g_settings.aimbot_enabled || g_settings.triggerbot_enabled) {
        start_aimbot_thread();
    }

    g_weapon_icons.init(g_overlay.get_device());

    printf("[+] %s = menu | %s = master toggle | %s = exit\n",
           vk_name(g_settings.key_menu),
           vk_name(g_settings.key_master),
           vk_name(g_settings.key_exit));

    EntityReader entity_reader;
    bool prev_menu = g_settings.menu_open;
    int spec_tick = 0;
    bool was_aimbot_enabled = false;
    std::string last_map_name;

    g_overlay.set_interactive(g_settings.menu_open);

    while (g_running) {
        auto frame_start = std::chrono::high_resolution_clock::now();

        if (GetAsyncKeyState(g_settings.key_exit) & 1) break;

        if (GetAsyncKeyState(g_settings.key_menu) & 1) g_menu.toggle();
        if (GetAsyncKeyState(g_settings.key_master) & 1)
            g_settings.master_switch = !g_settings.master_switch;

        if (g_settings.menu_open != prev_menu) {
            g_overlay.set_interactive(g_settings.menu_open);
            if (!g_settings.menu_open)
                Config::save(CONFIG_PATH); // save once when menu closes
            prev_menu = g_settings.menu_open;
        }



        bool want_aimbot = (g_settings.aimbot_enabled || g_settings.triggerbot_enabled ||
                            g_settings.rcs_enabled) && g_settings.master_switch;
        if (want_aimbot && !was_aimbot_enabled) {
            start_aimbot_thread();
        }
        else if (!want_aimbot && was_aimbot_enabled) {
            stop_aimbot_thread();
        }
        was_aimbot_enabled = want_aimbot;

        if (!g_overlay.begin_frame()) break;
        g_menu.render();

        if (!g_settings.master_switch) {
            static constexpr int IDLE_FPS_CAP = 20;
            g_overlay.end_frame(0);
            limit_frame(frame_start, IDLE_FPS_CAP);
            continue;
        }

        FrameState state = entity_reader.read_frame(
            g_overlay.width, g_overlay.height);

        if (g_settings.aimbot_enabled || g_settings.triggerbot_enabled || g_settings.rcs_enabled)
        {
            AimbotFrame af{};
            af.view_matrix = state.view_matrix;
            af.local_x     = state.local.x;
            af.local_y     = state.local.y;
            af.local_z     = state.local.z;
            af.local_team  = state.local.team;
            af.local_pawn  = state.local.pawn;
            af.screen_w    = g_overlay.width;
            af.screen_h    = g_overlay.height;
            af.is_scoped   = state.local.is_scoped;
            af.local_player_index = state.local_player_index;
            af.local_weapon_def_index = state.local_weapon_def_index;
            af.aim_punch   = state.local.aim_punch;
            af.shots_fired = state.local.shots_fired;
            af.local_velocity = state.local.velocity;

            if (state.local.camera.valid)
            {
                af.eye_origin   = state.local.camera.origin;
                af.view_angles  = state.local.camera.angles;
                af.camera_fov   = state.local.camera.fov;
                af.camera_valid = true;
            }
            else
            {
                af.eye_origin  = { state.local.x,
                                   state.local.y,
                                   state.local.z + 64.0f };
                af.camera_fov  = 90.0f;
                af.camera_valid = false;
            }

            for (int i = 1; i < EntityList::MAX_PLAYERS; i++)
            {
                const auto& p = state.players[i];
                af.targets[i].valid      = p.valid;
                af.targets[i].team       = p.team;
                af.targets[i].health     = p.health;
                af.targets[i].head_pos   = p.head_world;
                af.targets[i].neck_pos   = p.neck_world;
                af.targets[i].chest_pos  = p.chest_world;
                af.targets[i].pelvis_pos     = p.pelvis_world;
                af.targets[i].velocity       = p.velocity;
                af.targets[i].bSpottedByMask = p.bSpottedByMask;
            }

            g_aimbot_data.publish(af);
        }

        if (state.local.observer_pawn != 0 && state.local.pawn != 0 && !state.map_name.empty() && state.map_name != "<empty>" && state.map_name != last_map_name) {
            printf( "[+] Map change: %s -> %s\n", last_map_name.data(), state.map_name.data());
            last_map_name = state.map_name;
            g_bvh.clear();
            printf( "[+] Parsing bvh for %s\n", last_map_name.data());
            g_bvh.parse();
            printf( "[+] Bvh parsed\n" );
        }

        float fwd_x = state.view_matrix.m[2][0];
        float fwd_y = state.view_matrix.m[2][1];
        float fwd_z = state.view_matrix.m[2][2];

        float view_pitch_deg = -asinf(std::clamp(fwd_z, -1.0f, 1.0f))
                                * 180.0f / 3.14159265f;
        float view_yaw_deg   =  atan2f(fwd_y, fwd_x)
                                * 180.0f / 3.14159265f;

        g_grenades.set_view_matrix(state.view_matrix);
        g_grenades.update(state.local.x, state.local.y, state.local.z,
                          view_pitch_deg, view_yaw_deg, state.map_name);

        if (state.entity_list) {
            g_world_esp.update(state.entity_list,
                { state.local.x, state.local.y, state.local.z });

            if (++spec_tick >= 15) {
                spec_tick = 0;
                g_spectators.update(state.entity_list,
                    state.local.pawn, state.local.controller);
            }
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();

        float cam_fov = state.local.camera.valid ? state.local.camera.fov : 90.f;
        draw_aimbot_fov(draw, g_overlay.width, g_overlay.height, cam_fov);

        for (int i = 1; i < EntityList::MAX_PLAYERS; i++) {
            g_esp.draw_player(draw, state.players[i], state.local.team,
                g_overlay.width, g_overlay.height, i, state.local.is_scoped);
        }

        // C4 ESP (carried, dropped, planted)
        {
            static PlantedC4State c4_cache;
            static int c4_cache_tick = 0;
            static bool c4_debug_printed = false;
            if (++c4_cache_tick >= 5) {
                PlantedC4State fresh = read_planted_c4();
                if (!fresh.valid) {
                    c4_cache.valid = false;
                    c4_debug_printed = false;
                } else {
                    c4_cache = fresh;
                    if (!c4_debug_printed) {
                        printf("[+] C4 planted at site %c (0x%llX)\n",
                            c4_cache.bomb_site == 0 ? 'A' : 'B',
                            (unsigned long long)c4_cache.entity);
                        c4_debug_printed = true;
                    }
                }
                c4_cache_tick = 0;
            }
            if (c4_cache.valid) {
                draw_c4_esp(draw, c4_cache, g_overlay.width, g_overlay.height, state.view_matrix);
                draw_bomb_timer(c4_cache);
            }

            DroppedC4State dropped = read_dropped_c4();
            if (dropped.valid) {
                bool someone_has_c4 = false;
                for (int i = 1; i < EntityList::MAX_PLAYERS; i++) {
                    if (state.players[i].has_c4) {
                        someone_has_c4 = true;
                        break;
                    }
                }
                if (!someone_has_c4)
                    draw_dropped_c4_esp(draw, dropped, g_overlay.width, g_overlay.height, state.view_matrix);
            }
        }

        g_grenades.render_popups();
        g_grenades.draw(draw,
                        state.local.x, state.local.y, state.local.z,
                        g_overlay.width, g_overlay.height);

        g_world_esp.draw(draw, state.view_matrix,
                         g_overlay.width, g_overlay.height,
                         state.local.x, state.local.y, state.local.z);

        g_radar.draw(draw, state.radar_players, EntityList::MAX_PLAYERS,
            state.local.x, state.local.y, state.local.yaw, state.local.team,
            state.map_scale, g_overlay.width, g_overlay.height);

        g_spectators.draw(g_overlay.width);

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        Crosshair::Config xhair_cfg = {
            g_settings.crosshair_enabled && g_settings.master_switch,
            g_settings.crosshair_shape,
            g_settings.crosshair_size,
            g_settings.crosshair_gap,
            g_settings.crosshair_thickness,
            float4_to_col(g_settings.crosshair_color),
            g_settings.crosshair_outline,
            g_settings.crosshair_outline_thickness,
            float4_to_col(g_settings.crosshair_outline_color),
            g_settings.crosshair_dot,
            g_settings.crosshair_dot_size,
        };
        g_crosshair.draw(fg, g_overlay.width, g_overlay.height, xhair_cfg);

        g_overlay.end_frame(g_settings.use_vsync ? 1 : 0);

        if (!g_settings.use_vsync) {
            float fps = g_settings.target_fps;
            if (fps > 144.f) fps = 144.f;
            limit_frame(frame_start, fps);
        }
    }

    // Clean exit — save config BEFORE destroying overlay/graphics
    printf("\n[*] Shutting down...\n");
    save_and_exit();
    g_weapon_icons.shutdown();
    g_overlay.shutdown();
    cleanup_on_exit();
    CoUninitialize();
    printf("[*] Done. Press any key to exit...\n");
    system("pause>nul");
    return 0;
}