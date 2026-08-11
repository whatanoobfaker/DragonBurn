#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <urlmon.h>
#include <wininet.h>
#include <chrono>
#include <fstream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "memory/imemory.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")

struct Offsets {
    struct {
        uint32_t dwEntityList, dwViewMatrix, dwViewRender, dwLocalPlayerPawn, dwLocalPlayerController, dwGlobalVars, dwPlantedC4, dwWeaponC4;
        uint32_t dwGameEntitySystem_highestEntityIndex;
    } client;
    struct {
        uint32_t dwBuildNumber;
    } engine2;
    struct {
        uint32_t m_iTeamNum, m_pGameSceneNode, m_iHealth;
        uint32_t m_lifeState;
        uint32_t m_vecAbsVelocity;
    } C_BaseEntity;
    struct {
        uint32_t m_vecAbsOrigin;
    } CGameSceneNode;
    struct {
        uint32_t m_modelState;
    } CSkeletonInstance;
    struct {
        uint32_t m_hPlayerPawn;
        uint32_t m_hPawn;
        uint32_t m_sSanitizedPlayerName;
        uint32_t m_hObserverPawn;
    } CCSPlayerController;
    struct {
        uint32_t m_pObserverServices;
    } C_BasePlayerPawn;
    struct {
        uint32_t m_hObserverTarget;
    } CPlayer_ObserverServices;
    struct {
        uint32_t m_bIsScoped, m_entitySpottedState;
        uint32_t m_iShotsFired;
        uint32_t m_pAimPunchServices;
    } C_CSPlayerPawn;
    struct {
        uint32_t m_predictableBaseAngle; // aim punch angles
    } CCSPlayer_AimPunchServices;
    struct {
        uint32_t m_vecViewOffset;
    } C_BaseModelEntity;
    struct {
        uint32_t m_pWeaponServices;
        uint32_t m_flFlashDuration;
    } C_CSPlayerPawnBase;
    struct {
        uint32_t m_hActiveWeapon;
        uint32_t m_hMyWeapons;
    } CPlayer_WeaponServices;
    struct {
        uint32_t m_AttributeManager;
    } C_EconEntity;
    struct {
        uint32_t m_Item;
    } C_AttributeContainer;
    struct {
        uint32_t m_iItemDefinitionIndex;
    } C_EconItemView;
    struct {
        uint32_t m_bSpotted;
        uint32_t m_bSpottedByMask;
    } EntitySpottedState_t;

    struct {
        uint32_t m_bBeingDefused;
        uint32_t m_flDefuseCountDown;
        uint32_t m_nBombSite;
        uint32_t m_flC4Blow;
    } C4;

    struct {
        uint32_t m_pEntity;
    } CEntityInstance;
    struct {
        uint32_t m_designerName;
    } CEntityIdentity;

    struct {
        uint32_t m_vInitialPosition;
        uint32_t m_vInitialVelocity;
        uint32_t m_nBounces;
        uint32_t m_flSpawnTime;
        uint32_t vecLastTrailLinePos;
        uint32_t m_arrTrajectoryTrailPoints;
        uint32_t m_bExplodeEffectBegan;
    } C_BaseCSGrenadeProjectile;

    struct {
        uint32_t m_nSmokeEffectTickBegin;
        uint32_t m_bDidSmokeEffect;
        uint32_t m_vSmokeDetonationPos;
    } C_SmokeGrenadeProjectile;

    struct {
        uint32_t m_bIsIncGrenade;
    } C_MolotovProjectile;

    struct {
        uint32_t m_fireCount;
        uint32_t m_nFireEffectTickBegin;
        uint32_t m_firePositions;
        uint32_t m_bFireIsBurning;
    } C_Inferno;

    bool load(const std::string& offsets_path, const std::string& client_dll_path) {
        // Smart cache: do NOT hit GitHub on every launch (rate-limit hell while testing).
        // Refresh when cache is missing/stale, CS2 client.dll changed, build number changed,
        // or the functional test proves offsets are wrong.
        const auto meta_path = cache_meta_path(offsets_path);
        const auto fingerprint = fingerprint_remote_module(L"client.dll");

        bool fetched = false;
        std::string fetch_reason;
        if (should_fetch_remote(offsets_path, client_dll_path, meta_path, fingerprint, fetch_reason)) {
            printf("[*] Offset refresh needed: %s\n", fetch_reason.c_str());
            fetched = fetch_remote_offsets(offsets_path, client_dll_path);
            if (!fetched) {
                if (std::filesystem::exists(offsets_path) && std::filesystem::exists(client_dll_path)) {
                    printf("[!] Remote fetch failed — falling back to local cache\n");
                } else {
                    printf("[-] No remote offsets and no local cache\n");
                    return false;
                }
            }
        } else {
            printf("[*] Using cached offsets (%s)\n", fetch_reason.c_str());
        }

        if (!parse_offsets(offsets_path, client_dll_path)) {
            printf("[!] Failed to parse offsets after download/cache\n");
            if (!fetched) {
                printf("[*] Cache may be corrupt — forcing remote pull...\n");
                if (fetch_remote_offsets(offsets_path, client_dll_path) &&
                    parse_offsets(offsets_path, client_dll_path)) {
                    fetched = true;
                } else {
                    return false;
                }
            } else {
                return false;
            }
        }

        // If CS2 build number changed since last successful run, pull again (a2x may have new dumps).
        const uint32_t live_build = read_live_build_number();
        const uint32_t cached_build = read_meta_build_number(meta_path);
        if (live_build != 0 && cached_build != 0 && live_build != cached_build && !fetched) {
            printf("[!] CS2 build changed (%u -> %u) — fetching fresh a2x dumps...\n",
                   cached_build, live_build);
            if (fetch_remote_offsets(offsets_path, client_dll_path) &&
                parse_offsets(offsets_path, client_dll_path)) {
                fetched = true;
            } else {
                printf("[!] Fetch after build change failed; continuing with existing cache\n");
            }
        }

        auto result = functional_test();
        if (result == TestResult::OFFSETS_WRONG) {
            printf("[!] Offsets look wrong against live game — re-fetching from GitHub...\n");
            if (!fetch_remote_offsets(offsets_path, client_dll_path))
                return false;
            if (!parse_offsets(offsets_path, client_dll_path))
                return false;
            fetched = true;
            result = functional_test();
            if (result == TestResult::OFFSETS_WRONG) {
                printf("[!] Offsets still invalid. a2x dump may lag the game update, or patterns broke.\n");
                return false;
            }
        }

        if (result == TestResult::NO_PLAYERS) {
            printf("[*] Offsets parsed OK but no players found (you may not be in a match)\n");
            printf("[*] Proceeding — offsets will be validated when players are present\n");
        } else {
            printf("[+] Offsets validated: successfully read player data\n");
        }

        write_cache_meta(meta_path, fingerprint, live_build != 0 ? live_build : cached_build, fetched);
        return true;
    }

private:
    enum class TestResult { OK, NO_PLAYERS, OFFSETS_WRONG };

    // How long cached a2x JSONs stay valid without a CS2 update signal.
    static constexpr int kCacheMaxAgeHours = 6;

    static constexpr const char* kOffsetsUrl =
        "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json";
    static constexpr const char* kClientDllUrl =
        "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json";

    struct DllFingerprint {
        std::string path;
        uint64_t size = 0;
        int64_t mtime = 0;
        bool ok = false;
    };

    static std::filesystem::path cache_meta_path(const std::string& offsets_path) {
        return std::filesystem::path(offsets_path).parent_path() / "cache_meta.json";
    }

    static int64_t unix_now() {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    static std::string wide_to_utf8(const wchar_t* w) {
        if (!w || !w[0]) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string out(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
        return out;
    }

    static DllFingerprint fingerprint_remote_module(const wchar_t* module_leaf) {
        DllFingerprint fp;
        if (!g_memory) return fp;
        const DWORD pid = g_memory->get_pid();
        if (!pid) return fp;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return fp;

        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        bool found = false;
        wchar_t path[MAX_PATH]{};
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, module_leaf) == 0) {
                    wcsncpy_s(path, me.szExePath, _TRUNCATE);
                    found = true;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        if (!found || !path[0]) return fp;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
            return fp;

        ULARGE_INTEGER sz{};
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        ULARGE_INTEGER mt{};
        mt.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;

        fp.path = wide_to_utf8(path);
        fp.size = sz.QuadPart;
        fp.mtime = static_cast<int64_t>(mt.QuadPart);
        fp.ok = true;
        return fp;
    }

    static uint32_t read_meta_build_number(const std::filesystem::path& meta_path) {
        try {
            if (!std::filesystem::exists(meta_path)) return 0;
            std::ifstream in(meta_path);
            nlohmann::json j;
            in >> j;
            return j.value("build_number", 0u);
        } catch (...) {
            return 0;
        }
    }

    uint32_t read_live_build_number() const {
        if (!engine2.dwBuildNumber || !g_memory) return 0;
        const uintptr_t base = g_memory->get_modules().engine2;
        if (!base) return 0;
        return g_memory->read<uint32_t>(base + engine2.dwBuildNumber);
    }

    static bool should_fetch_remote(
        const std::string& offsets_path,
        const std::string& client_dll_path,
        const std::filesystem::path& meta_path,
        const DllFingerprint& fingerprint,
        std::string& reason_out)
    {
        if (!std::filesystem::exists(offsets_path) || !std::filesystem::exists(client_dll_path)) {
            reason_out = "no local cache";
            return true;
        }

        nlohmann::json meta;
        bool have_meta = false;
        try {
            if (std::filesystem::exists(meta_path)) {
                std::ifstream in(meta_path);
                in >> meta;
                have_meta = true;
            }
        } catch (...) {
            have_meta = false;
        }

        if (!have_meta) {
            reason_out = "missing cache_meta.json";
            return true;
        }

        const int64_t last = meta.value("last_fetch_unix", (int64_t)0);
        const int max_age_h = meta.value("max_age_hours", kCacheMaxAgeHours);
        const int64_t age_s = unix_now() - last;
        if (last <= 0 || age_s >= static_cast<int64_t>(max_age_h) * 3600) {
            char buf[96];
            snprintf(buf, sizeof(buf), "cache older than %dh (age=%.1fh)",
                     max_age_h, age_s / 3600.0);
            reason_out = buf;
            return true;
        }

        if (fingerprint.ok) {
            const uint64_t old_size = meta.value("client_dll_size", (uint64_t)0);
            const int64_t old_mtime = meta.value("client_dll_mtime", (int64_t)0);
            const std::string old_path = meta.value("client_dll_path", std::string{});
            if (old_size != fingerprint.size || old_mtime != fingerprint.mtime ||
                (!old_path.empty() && !fingerprint.path.empty() && old_path != fingerprint.path))
            {
                reason_out = "client.dll on disk changed (CS2 likely updated)";
                return true;
            }
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "fresh — age %.1fh / max %dh; client.dll unchanged",
                 age_s / 3600.0, max_age_h);
        reason_out = buf;
        return false;
    }

    static void write_cache_meta(
        const std::filesystem::path& meta_path,
        const DllFingerprint& fingerprint,
        uint32_t build_number,
        bool just_fetched)
    {
        try {
            nlohmann::json meta;
            int64_t last_fetch = unix_now();
            if (!just_fetched && std::filesystem::exists(meta_path)) {
                try {
                    std::ifstream in(meta_path);
                    nlohmann::json old;
                    in >> old;
                    last_fetch = old.value("last_fetch_unix", last_fetch);
                    if (build_number == 0)
                        build_number = old.value("build_number", 0u);
                } catch (...) {}
            }

            meta["last_fetch_unix"] = last_fetch;
            meta["max_age_hours"] = kCacheMaxAgeHours;
            meta["build_number"] = build_number;
            if (fingerprint.ok) {
                meta["client_dll_path"] = fingerprint.path;
                meta["client_dll_size"] = fingerprint.size;
                meta["client_dll_mtime"] = fingerprint.mtime;
            }
            meta["updated_unix"] = unix_now();

            std::filesystem::create_directories(meta_path.parent_path());
            std::ofstream out(meta_path);
            out << meta.dump(2);
            if (just_fetched)
                printf("[+] Offset cache meta saved (TTL %dh, build=%u)\n",
                       kCacheMaxAgeHours, build_number);
        } catch (const std::exception& e) {
            printf("[!] Failed to write cache meta: %s\n", e.what());
        }
    }

    bool download_url_to_file(const char* url, const std::filesystem::path& dest) {
        std::filesystem::create_directories(dest.parent_path());

        std::filesystem::path tmp = dest;
        tmp += ".tmp";

        DeleteUrlCacheEntryA(url);

        HRESULT hr = URLDownloadToFileA(nullptr, url, tmp.string().c_str(), 0, nullptr);
        if (FAILED(hr)) {
            printf("[!] Download failed (%s) HRESULT=0x%08lX\n", url, (unsigned long)hr);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }

        std::error_code ec;
        auto sz = std::filesystem::file_size(tmp, ec);
        if (ec || sz < 32) {
            printf("[!] Downloaded file looks empty/invalid: %s\n", tmp.string().c_str());
            std::filesystem::remove(tmp, ec);
            return false;
        }

        {
            std::ifstream in(tmp);
            char c = 0;
            in >> c;
            if (c != '{') {
                printf("[!] Downloaded content is not JSON object: %s\n", dest.filename().string().c_str());
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }

        std::filesystem::remove(dest, ec);
        std::filesystem::rename(tmp, dest, ec);
        if (ec) {
            try {
                std::filesystem::copy_file(tmp, dest, std::filesystem::copy_options::overwrite_existing);
                std::filesystem::remove(tmp);
            } catch (const std::exception& e) {
                printf("[!] Failed to finalize %s: %s\n", dest.string().c_str(), e.what());
                return false;
            }
        }

        printf("[+] Downloaded %s (%llu bytes)\n", dest.filename().string().c_str(),
               (unsigned long long)sz);
        return true;
    }

    bool fetch_remote_offsets(const std::string& offsets_path, const std::string& client_dll_path) {
        printf("[*] Fetching offsets from a2x/cs2-dumper (GitHub)...\n");
        bool ok_off = download_url_to_file(kOffsetsUrl, offsets_path);
        bool ok_cli = download_url_to_file(kClientDllUrl, client_dll_path);
        if (ok_off && ok_cli) {
            printf("[+] Remote offsets ready\n");
            return true;
        }
        printf("[!] Remote fetch incomplete (offsets=%s, client_dll=%s)\n",
               ok_off ? "ok" : "fail", ok_cli ? "ok" : "fail");
        return false;
    }

    bool parse_offsets(const std::string& offsets_path, const std::string& client_dll_path) {
        try {
            nlohmann::json oj, cj;
            {
                std::ifstream f(offsets_path);
                if (!f) return false;
                f >> oj;
            }
            {
                std::ifstream f(client_dll_path);
                if (!f) return false;
                f >> cj;
            }

            auto& cl = oj["client.dll"];
            client.dwEntityList = cl["dwEntityList"];
            client.dwViewMatrix = cl["dwViewMatrix"];
            client.dwViewRender = cl["dwViewRender"];
            client.dwLocalPlayerPawn = cl["dwLocalPlayerPawn"];
            client.dwLocalPlayerController = cl["dwLocalPlayerController"];
            client.dwGlobalVars = cl["dwGlobalVars"];
            client.dwPlantedC4 = cl.value("dwPlantedC4", 0u);
            client.dwWeaponC4 = cl.value("dwWeaponC4", 0u);
            client.dwGameEntitySystem_highestEntityIndex =
                cl.value("dwGameEntitySystem_highestEntityIndex", 0u);

            engine2.dwBuildNumber = 0;
            if (oj.contains("engine2.dll"))
                engine2.dwBuildNumber = oj["engine2.dll"].value("dwBuildNumber", 0u);

            auto& cs = cj["client.dll"]["classes"];
            C_BaseEntity.m_iTeamNum = cs["C_BaseEntity"]["fields"]["m_iTeamNum"];
            C_BaseEntity.m_pGameSceneNode = cs["C_BaseEntity"]["fields"]["m_pGameSceneNode"];
            C_BaseEntity.m_iHealth = cs["C_BaseEntity"]["fields"]["m_iHealth"];
            C_BaseEntity.m_lifeState = cs["C_BaseEntity"]["fields"].value("m_lifeState", 0u);
            C_BaseEntity.m_vecAbsVelocity = cs["C_BaseEntity"]["fields"].value("m_vecAbsVelocity", 0u);
            CGameSceneNode.m_vecAbsOrigin = cs["CGameSceneNode"]["fields"]["m_vecAbsOrigin"];
            CSkeletonInstance.m_modelState = cs["CSkeletonInstance"]["fields"]["m_modelState"];

            CCSPlayerController.m_hPlayerPawn =
                cs["CCSPlayerController"]["fields"]["m_hPlayerPawn"];
            CCSPlayerController.m_sSanitizedPlayerName =
                cs["CCSPlayerController"]["fields"]["m_sSanitizedPlayerName"];
            CCSPlayerController.m_hPawn =
                cs["CBasePlayerController"]["fields"]["m_hPawn"];
            CCSPlayerController.m_hObserverPawn =
                cs["CCSPlayerController"]["fields"].value("m_hObserverPawn", 0u);
            if (!CCSPlayerController.m_hObserverPawn) {
                printf("[!] m_hObserverPawn missing from dump — observer/map triggers degraded\n");
            }

            C_BasePlayerPawn.m_pObserverServices =
                cs["C_BasePlayerPawn"]["fields"]["m_pObserverServices"];
            CPlayer_ObserverServices.m_hObserverTarget =
                cs["CPlayer_ObserverServices"]["fields"]["m_hObserverTarget"];

            C_CSPlayerPawn.m_bIsScoped = cs["C_CSPlayerPawn"]["fields"]["m_bIsScoped"];
            C_CSPlayerPawn.m_iShotsFired = cs["C_CSPlayerPawn"]["fields"].value("m_iShotsFired", 0u);
            C_CSPlayerPawn.m_pAimPunchServices = cs["C_CSPlayerPawn"]["fields"].value("m_pAimPunchServices", 0u);
            C_BaseModelEntity.m_vecViewOffset =
                cs["C_BaseModelEntity"]["fields"]["m_vecViewOffset"];

            if (cs.contains("CCSPlayer_AimPunchServices")) {
                CCSPlayer_AimPunchServices.m_predictableBaseAngle =
                    cs["CCSPlayer_AimPunchServices"]["fields"].value("m_predictableBaseAngle", 0u);
            }

            C_CSPlayerPawnBase.m_pWeaponServices = cs["C_BasePlayerPawn"]["fields"]["m_pWeaponServices"];
            C_CSPlayerPawnBase.m_flFlashDuration = cs["C_CSPlayerPawnBase"]["fields"]["m_flFlashDuration"];
            CPlayer_WeaponServices.m_hActiveWeapon = cs["CPlayer_WeaponServices"]["fields"]["m_hActiveWeapon"];
            CPlayer_WeaponServices.m_hMyWeapons = cs["CPlayer_WeaponServices"]["fields"]["m_hMyWeapons"];

            C_EconEntity.m_AttributeManager = cs["C_EconEntity"]["fields"]["m_AttributeManager"];
            C_AttributeContainer.m_Item = cs["C_AttributeContainer"]["fields"]["m_Item"];
            C_EconItemView.m_iItemDefinitionIndex = cs["C_EconItemView"]["fields"]["m_iItemDefinitionIndex"];

            C_CSPlayerPawn.m_entitySpottedState = cs["C_CSPlayerPawn"]["fields"]["m_entitySpottedState"];
            EntitySpottedState_t.m_bSpotted = cs["EntitySpottedState_t"]["fields"]["m_bSpotted"];
            EntitySpottedState_t.m_bSpottedByMask = cs["EntitySpottedState_t"]["fields"]["m_bSpottedByMask"];

            if (cs.contains("C_PlantedC4") && cs["C_PlantedC4"].contains("fields")) {
                auto& c4f = cs["C_PlantedC4"]["fields"];
                C4.m_bBeingDefused = c4f.value("m_bBeingDefused", 0u);
                C4.m_flDefuseCountDown = c4f.value("m_flDefuseCountDown", 0u);
                C4.m_nBombSite = c4f.value("m_nBombSite", 0u);
                C4.m_flC4Blow = c4f.value("m_flC4Blow", 0u);
            }

            CEntityInstance.m_pEntity = cs["CEntityInstance"]["fields"].value("m_pEntity", 0u);
            CEntityIdentity.m_designerName = cs["CEntityIdentity"]["fields"].value("m_designerName", 0u);

            if (cs.contains("C_BaseCSGrenadeProjectile")) {
                auto& gf = cs["C_BaseCSGrenadeProjectile"]["fields"];
                C_BaseCSGrenadeProjectile.m_vInitialPosition = gf.value("m_vInitialPosition", 0u);
                C_BaseCSGrenadeProjectile.m_vInitialVelocity = gf.value("m_vInitialVelocity", 0u);
                C_BaseCSGrenadeProjectile.m_nBounces = gf.value("m_nBounces", 0u);
                C_BaseCSGrenadeProjectile.m_flSpawnTime = gf.value("m_flSpawnTime", 0u);
                C_BaseCSGrenadeProjectile.vecLastTrailLinePos = gf.value("vecLastTrailLinePos", 0u);
                C_BaseCSGrenadeProjectile.m_arrTrajectoryTrailPoints = gf.value("m_arrTrajectoryTrailPoints", 0u);
                C_BaseCSGrenadeProjectile.m_bExplodeEffectBegan = gf.value("m_bExplodeEffectBegan", 0u);
            }
            if (cs.contains("C_SmokeGrenadeProjectile")) {
                auto& sf = cs["C_SmokeGrenadeProjectile"]["fields"];
                C_SmokeGrenadeProjectile.m_nSmokeEffectTickBegin = sf.value("m_nSmokeEffectTickBegin", 0u);
                C_SmokeGrenadeProjectile.m_bDidSmokeEffect = sf.value("m_bDidSmokeEffect", 0u);
                C_SmokeGrenadeProjectile.m_vSmokeDetonationPos = sf.value("m_vSmokeDetonationPos", 0u);
            }
            if (cs.contains("C_MolotovProjectile")) {
                C_MolotovProjectile.m_bIsIncGrenade =
                    cs["C_MolotovProjectile"]["fields"].value("m_bIsIncGrenade", 0u);
            }
            if (cs.contains("C_Inferno")) {
                auto& inf = cs["C_Inferno"]["fields"];
                C_Inferno.m_fireCount = inf.value("m_fireCount", 0u);
                C_Inferno.m_nFireEffectTickBegin = inf.value("m_nFireEffectTickBegin", 0u);
                C_Inferno.m_firePositions = inf.value("m_firePositions", 0u);
                C_Inferno.m_bFireIsBurning = inf.value("m_bFireIsBurning", 0u);
            }

            return true;
        } catch (const std::exception& e) {
            printf("[!] Offset parse error: %s\n", e.what());
            return false;
        }
    }

    TestResult functional_test() {
        uintptr_t client_base = g_memory->get_client_base();
        if (!client_base) {
            printf("[!] client_base is 0\n");
            return TestResult::OFFSETS_WRONG;
        }

        uintptr_t entity_list = g_memory->read<uintptr_t>(client_base + client.dwEntityList);
        if (!entity_list) {
            printf("[!] entity_list is null (offset 0x%X)\n", client.dwEntityList);
            return TestResult::OFFSETS_WRONG;
        }

        uintptr_t first_page = g_memory->read<uintptr_t>(entity_list + 16);
        if (!first_page) {
            return TestResult::NO_PLAYERS;
        }

        int valid_players = 0;
        int bogus_reads = 0;

        for (int i = 1; i < 64; i++) {
            uintptr_t controller = g_memory->read<uintptr_t>(first_page + 112 * (i & 0x1FF));
            if (!controller) continue;

            uint32_t pawn_handle = g_memory->read<uint32_t>(
                controller + CCSPlayerController.m_hPawn);
            if (!pawn_handle) {
                pawn_handle = g_memory->read<uint32_t>(
                    controller + CCSPlayerController.m_hPlayerPawn);
            }
            if (!pawn_handle) continue;

            uintptr_t pawn_page = g_memory->read<uintptr_t>(
                entity_list + 8 * ((pawn_handle & 0x7FFF) >> 9) + 16);
            if (!pawn_page) continue;

            uintptr_t pawn = g_memory->read<uintptr_t>(
                pawn_page + 112 * (pawn_handle & 0x1FF));
            if (!pawn) continue;

            int team = g_memory->read<int>(pawn + C_BaseEntity.m_iTeamNum);
            int health = g_memory->read<int>(pawn + C_BaseEntity.m_iHealth);

            if (team >= 0 && team <= 3) {
                if (health >= 0 && health <= 100) {
                    valid_players++;
                } else if (health > 100 || health < -1) {
                    bogus_reads++;
                }
            } else {
                bogus_reads++;
            }
        }

        if (valid_players > 0 && bogus_reads <= valid_players) {
            printf("[+] Found %d valid players in functional test\n", valid_players);
            return TestResult::OK;
        }

        if (valid_players == 0 && bogus_reads == 0) {
            return TestResult::NO_PLAYERS;
        }

        printf("[!] Functional test failed: %d valid, %d bogus reads\n",
               valid_players, bogus_reads);
        return TestResult::OFFSETS_WRONG;
    }
};

inline Offsets g_offsets;
