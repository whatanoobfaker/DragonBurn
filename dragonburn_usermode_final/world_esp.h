#pragma once
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <imgui.h>

#include "entity_utils.h"
#include "memory/imemory.h"
#include "offsets.h"
#include "overlay.h"
#include "settings.h"
#include "types.h"

enum class WorldNadeKind : uint8_t {
    He = 0,
    Flash,
    Smoke,
    Molotov,
    Decoy,
    Inferno,
    Unknown
};

struct WorldEspEntity {
    bool valid = false;
    WorldNadeKind kind = WorldNadeKind::Unknown;
    Vec3 origin{};
    Vec3 trail[48]{};
    int trail_count = 0;
    bool smoke_popped = false;
    Vec3 smoke_pos{};
    int fire_count = 0;
    Vec3 fires[32]{};
};

class WorldEsp {
public:
    void update(uintptr_t entity_list, const Vec3& local_pos) {
        if (!g_settings.master_switch || !g_settings.world_esp_enabled || !entity_list) {
            m_count = 0;
            return;
        }

        // Refresh a few times per second — entity scan is RPM-heavy
        if (++m_tick < 3) return;
        m_tick = 0;

        m_count = 0;

        int highest = 2048;
        if (g_offsets.client.dwGameEntitySystem_highestEntityIndex) {
            int hi = g_memory->read<int>(
                entity_list + g_offsets.client.dwGameEntitySystem_highestEntityIndex);
            if (hi > 64 && hi < 8192)
                highest = hi;
        }

        const float max_dist_sq = g_settings.world_esp_max_dist * g_settings.world_esp_max_dist;

        // Skip player slots (1..64); scan world entities
        for (int idx = 65; idx <= highest && m_count < 96; idx++) {
            uintptr_t ent = EntityList::resolve_handle(entity_list, static_cast<uint32_t>(idx));
            if (!ent) continue;

            char name[64]{};
            if (!read_designer_name(ent, name, sizeof(name)))
                continue;

            WorldNadeKind kind = classify(name);
            if (kind == WorldNadeKind::Unknown) continue;

            if (kind == WorldNadeKind::Inferno && !g_settings.world_esp_inferno) continue;
            if (kind != WorldNadeKind::Inferno && !g_settings.world_esp_projectiles &&
                !g_settings.world_esp_smoke && !g_settings.world_esp_trails)
                continue;

            uintptr_t scene = g_memory->read<uintptr_t>(
                ent + g_offsets.C_BaseEntity.m_pGameSceneNode);
            if (!scene) continue;

            Vec3 origin = g_memory->read<Vec3>(
                scene + g_offsets.CGameSceneNode.m_vecAbsOrigin);

            float dx = origin.x - local_pos.x;
            float dy = origin.y - local_pos.y;
            float dz = origin.z - local_pos.z;
            if (dx * dx + dy * dy + dz * dz > max_dist_sq)
                continue;

            WorldEspEntity& out = m_ents[m_count];
            out = {};
            out.valid = true;
            out.kind = kind;
            out.origin = origin;

            if (kind == WorldNadeKind::Smoke && g_settings.world_esp_smoke) {
                if (g_offsets.C_SmokeGrenadeProjectile.m_bDidSmokeEffect) {
                    out.smoke_popped = g_memory->read<bool>(
                        ent + g_offsets.C_SmokeGrenadeProjectile.m_bDidSmokeEffect);
                }
                if (out.smoke_popped && g_offsets.C_SmokeGrenadeProjectile.m_vSmokeDetonationPos) {
                    out.smoke_pos = g_memory->read<Vec3>(
                        ent + g_offsets.C_SmokeGrenadeProjectile.m_vSmokeDetonationPos);
                } else {
                    out.smoke_pos = origin;
                }
            }

            if (kind == WorldNadeKind::Inferno && g_settings.world_esp_inferno) {
                read_inferno(ent, out);
            }

            if (g_settings.world_esp_trails && kind != WorldNadeKind::Inferno) {
                read_trail(ent, out);
            }

            // Skip dead/empty infernos
            if (kind == WorldNadeKind::Inferno && out.fire_count <= 0)
                continue;

            m_count++;
        }
    }

    void draw(ImDrawList* draw, const Matrix4x4& vm, int sw, int sh,
              float local_x, float local_y, float local_z) const
    {
        if (!g_settings.master_switch || !g_settings.world_esp_enabled || !draw)
            return;

        ImFont* font = g_overlay.esp_font;
        float fs = g_settings.esp_font_atlas_size * 0.85f;
        if (fs < 11.f) fs = 11.f;

        for (int i = 0; i < m_count; i++) {
            const WorldEspEntity& e = m_ents[i];
            if (!e.valid) continue;

            ImU32 col = color_for(e.kind);

            if (e.kind == WorldNadeKind::Inferno) {
                for (int f = 0; f < e.fire_count; f++) {
                    ImVec2 s;
                    if (!w2s(e.fires[f], vm, sw, sh, s)) continue;
                    draw->AddCircleFilled(s, 3.5f, col, 8);
                }
                if (g_settings.world_esp_labels && e.fire_count > 0) {
                    ImVec2 s;
                    if (w2s(e.fires[0], vm, sw, sh, s))
                        draw_label(draw, font, fs, s, "Molotov", col);
                }
                continue;
            }

            if (e.kind == WorldNadeKind::Smoke && e.smoke_popped && g_settings.world_esp_smoke) {
                ImVec2 s;
                if (w2s(e.smoke_pos, vm, sw, sh, s)) {
                    draw->AddCircle(s, 18.f, col, 24, 2.0f);
                    draw->AddCircleFilled(s, 4.f, col);
                    if (g_settings.world_esp_labels)
                        draw_label(draw, font, fs, s, "Smoke", col);
                }
                continue;
            }

            if (!g_settings.world_esp_projectiles && !g_settings.world_esp_trails)
                continue;

            // Trail polyline
            if (g_settings.world_esp_trails && e.trail_count >= 2) {
                ImVec2 prev{};
                bool have_prev = false;
                for (int t = 0; t < e.trail_count; t++) {
                    ImVec2 s;
                    if (!w2s(e.trail[t], vm, sw, sh, s)) {
                        have_prev = false;
                        continue;
                    }
                    if (have_prev)
                        draw->AddLine(prev, s, col, 2.0f);
                    prev = s;
                    have_prev = true;
                }
            } else if (g_settings.world_esp_trails &&
                       g_offsets.C_BaseCSGrenadeProjectile.m_vInitialPosition)
            {
                // Fallback: initial → current
                // (origin already current; initial read into trail[0] when possible)
            }

            if (g_settings.world_esp_projectiles) {
                ImVec2 s;
                if (w2s(e.origin, vm, sw, sh, s)) {
                    draw->AddCircleFilled(s, 5.f, col, 12);
                    draw->AddCircle(s, 7.f, IM_COL32(0, 0, 0, 180), 12, 1.5f);
                    if (g_settings.world_esp_labels)
                        draw_label(draw, font, fs, s, label_for(e.kind), col);

                    // Distance
                    float dist = sqrtf(
                        (e.origin.x - local_x) * (e.origin.x - local_x) +
                        (e.origin.y - local_y) * (e.origin.y - local_y) +
                        (e.origin.z - local_z) * (e.origin.z - local_z));
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.0fm", dist * 0.0254f);
                    if (font) {
                        ImVec2 ts = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.f, buf);
                        draw->AddText(font, fs * 0.9f,
                            { s.x - ts.x * 0.5f, s.y + 10.f },
                            IM_COL32(220, 220, 220, 200), buf);
                    }
                }
            }
        }
    }

private:
    WorldEspEntity m_ents[96]{};
    int m_count = 0;
    int m_tick = 0;

    static bool read_designer_name(uintptr_t ent, char* out, size_t n) {
        out[0] = 0;
        if (!ent || n < 2) return false;
        if (!g_offsets.CEntityInstance.m_pEntity || !g_offsets.CEntityIdentity.m_designerName)
            return false;

        uintptr_t identity = g_memory->read<uintptr_t>(
            ent + g_offsets.CEntityInstance.m_pEntity);
        if (!identity) return false;

        uintptr_t name_ptr = g_memory->read<uintptr_t>(
            identity + g_offsets.CEntityIdentity.m_designerName);
        if (!name_ptr) return false;

        if (!g_memory->read_raw(name_ptr, out, n - 1))
            return false;
        out[n - 1] = 0;
        return out[0] != 0;
    }

    static WorldNadeKind classify(const char* name) {
        if (!name || !name[0]) return WorldNadeKind::Unknown;
        // designer names are like "smokegrenade_projectile"
        if (strstr(name, "smokegrenade_projectile")) return WorldNadeKind::Smoke;
        if (strstr(name, "flashbang_projectile"))    return WorldNadeKind::Flash;
        if (strstr(name, "hegrenade_projectile"))    return WorldNadeKind::He;
        if (strstr(name, "molotov_projectile"))      return WorldNadeKind::Molotov;
        if (strstr(name, "incendiarygrenade_projectile")) return WorldNadeKind::Molotov;
        if (strstr(name, "decoy_projectile"))        return WorldNadeKind::Decoy;
        if (strcmp(name, "inferno") == 0)            return WorldNadeKind::Inferno;
        return WorldNadeKind::Unknown;
    }

    static void read_trail(uintptr_t ent, WorldEspEntity& out) {
        out.trail_count = 0;
        uint32_t off = g_offsets.C_BaseCSGrenadeProjectile.m_arrTrajectoryTrailPoints;
        if (!off) {
            // fallback: initial position → current
            if (g_offsets.C_BaseCSGrenadeProjectile.m_vInitialPosition) {
                out.trail[0] = g_memory->read<Vec3>(
                    ent + g_offsets.C_BaseCSGrenadeProjectile.m_vInitialPosition);
                out.trail[1] = out.origin;
                out.trail_count = 2;
            }
            return;
        }

        // Source 2 CUtlVector: ptr @ +0, size @ +0x10 (common)
        uintptr_t data = g_memory->read<uintptr_t>(ent + off);
        int size = g_memory->read<int>(ent + off + 0x10);
        if (size <= 0 || size > 512 || !data) {
            size = g_memory->read<int>(ent + off + 0x8);
            if (size <= 0 || size > 512 || !data) {
                if (g_offsets.C_BaseCSGrenadeProjectile.m_vInitialPosition) {
                    out.trail[0] = g_memory->read<Vec3>(
                        ent + g_offsets.C_BaseCSGrenadeProjectile.m_vInitialPosition);
                    out.trail[1] = out.origin;
                    out.trail_count = 2;
                }
                return;
            }
        }

        const int max_pts = static_cast<int>(sizeof(out.trail) / sizeof(out.trail[0]));
        int start = size > max_pts ? size - max_pts : 0;
        int count = size - start;
        if (count > max_pts) count = max_pts;

        for (int i = 0; i < count; i++) {
            out.trail[i] = g_memory->read<Vec3>(
                data + static_cast<uintptr_t>(start + i) * sizeof(Vec3));
        }
        out.trail_count = count;
    }

    static void read_inferno(uintptr_t ent, WorldEspEntity& out) {
        out.fire_count = 0;
        if (!g_offsets.C_Inferno.m_fireCount || !g_offsets.C_Inferno.m_firePositions)
            return;

        int count = g_memory->read<int>(ent + g_offsets.C_Inferno.m_fireCount);
        if (count <= 0 || count > 64) return;

        const int max_f = static_cast<int>(sizeof(out.fires) / sizeof(out.fires[0]));
        int n = count < max_f ? count : max_f;

        for (int i = 0; i < n; i++) {
            bool burning = true;
            if (g_offsets.C_Inferno.m_bFireIsBurning) {
                burning = g_memory->read<bool>(
                    ent + g_offsets.C_Inferno.m_bFireIsBurning + i);
            }
            if (!burning) continue;

            out.fires[out.fire_count++] = g_memory->read<Vec3>(
                ent + g_offsets.C_Inferno.m_firePositions +
                static_cast<uintptr_t>(i) * 16); // VectorWS stride
            if (out.fire_count >= max_f) break;
        }
    }

    static ImU32 color_for(WorldNadeKind k) {
        switch (k) {
        case WorldNadeKind::Smoke:   return float4_to_col(g_settings.world_esp_smoke_color);
        case WorldNadeKind::Molotov:
        case WorldNadeKind::Inferno: return float4_to_col(g_settings.world_esp_molotov_color);
        case WorldNadeKind::Flash:   return float4_to_col(g_settings.world_esp_flash_color);
        case WorldNadeKind::He:      return float4_to_col(g_settings.world_esp_he_color);
        default:                     return float4_to_col(g_settings.world_esp_trail_color);
        }
    }

    static const char* label_for(WorldNadeKind k) {
        switch (k) {
        case WorldNadeKind::Smoke:   return "Smoke";
        case WorldNadeKind::Molotov: return "Molly";
        case WorldNadeKind::Flash:   return "Flash";
        case WorldNadeKind::He:      return "HE";
        case WorldNadeKind::Decoy:   return "Decoy";
        case WorldNadeKind::Inferno: return "Fire";
        default:                     return "Nade";
        }
    }

    static void draw_label(ImDrawList* draw, ImFont* font, float fs,
                           ImVec2 s, const char* text, ImU32 col)
    {
        if (!font || !text) return;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, text);
        ImVec2 p{ s.x - ts.x * 0.5f, s.y - ts.y - 8.f };
        draw->AddText(font, fs, { p.x + 1.f, p.y + 1.f }, IM_COL32(0, 0, 0, 220), text);
        draw->AddText(font, fs, p, col, text);
    }
};

inline WorldEsp g_world_esp;
