# DragonBurn — Changes Since We Started

Detailed log of work done on this codebase in the Aug 11, 2026 session (and the continuation of that work). This is **not** a full project history from before that; it is what changed while hardening, polishing, and extending DragonBurn.

For build/run instructions, see [`README.md`](README.md).

---

## Timeline (what we worked through)

1. Fix clunky ImGui menu / overlay UI  
2. Repo / GitHub setup guidance (no code)  
3. Full technical review → fix **high** severity bugs  
4. Fix **medium** severity bugs  
5. Explain version fragility; switch offsets to **a2x remote dumps**  
6. Implement requested features: **RCS**, **richer aimbot**, **world ESP / trails / lineups**  
7. Stop requiring driver/mapper files beside the usermode exe; pull offsets **every launch**

---

## 1. UI polish (menu / overlay)

**Problem:** The menu felt broken — clipped content, bad drag, dead slider clicks, wrong panel heights, leftover branding.

### Fixes

| Area | What changed |
|------|----------------|
| Combat tab layout | Stopped double-subtracting the combat header height so the body wasn’t crushed |
| Content region | Enabled proper scrolling so long panels weren’t clipped |
| Window drag | Drag no longer depended on a `nullptr` HWND; uses ImGui `SetWindowPos` via `helpers/window_drag.cpp` |
| Sliders | Click-without-drag now applies a value (previously needed a drag gesture) |
| Panel rows | Corrected row counts / heights so panels sized to their contents |
| Keybinds | Layout and keybind dropdown style-stack fixes (popups no longer left ImGui style state dirty) |
| Top bar | Centering / alignment cleanup |
| Branding | Sidebar “C / ESP” portfolio leftover → **DragonBurn** (“D” mark + title) |
| Sidebar width | Increased (150 → 168) so labels fit cleanly |

### Main files

- `dragonburn_usermode_final/menu.h`
- `dragonburn_usermode_final/ui/widgets/widgets.cpp`
- `dragonburn_usermode_final/ui/sidebar/sidebar.cpp` / `sidebar.h`
- `dragonburn_usermode_final/ui/topbar/topbar.cpp`
- `dragonburn_usermode_final/helpers/window_drag.cpp` / `window_drag.h`
- `dragonburn_usermode_final/theme/layout.h` (as touched for spacing/sizing)

---

## 2. High-severity engineering fixes

After a codebase review, these were treated as must-fix before feature work.

### 2.1 Driver: honest memory reads

**Before:** On `MmCopyVirtualMemory` failure the driver zeroed the output buffer and still returned success. Usermode could not tell “read failed” from “value is actually zero.”

**After:**

- Real NTSTATUS is returned on failure  
- Partial copies are treated as failure  
- Usermode already treated failed IOCTLs as failed reads — it now sees the truth  

**File:** `dragonburn_driver/dragonburn_driver/driver.c`

### 2.2 Driver: IOCTL / device access control

**Before:** IOCTLs used `FILE_ANY_ACCESS`; any local process that opened the device could request reads.

**After:**

- IOCTLs require `FILE_READ_ACCESS | FILE_WRITE_ACCESS`  
- Device is **exclusive** (single handle)  
- First opener is bound as the only **client PID**; other PIDs get access denied  
- New `IOCTL_SET_TARGET_PID` — memory/module ops only allowed against the registered game PID  
- Matching definitions in usermode shared headers  

**Files:**

- `dragonburn_driver/dragonburn_driver/driver.c`
- `dragonburn_usermode_final/memory/shared.h`
- `dragonburn_usermode_final/memory/memory_driver.h` (registers target PID after attach)

### 2.3 Host mutation / cleanup (mapper + usermode loader)

**Before:**

- Broad Defender exclusion of `%TEMP%`  
- Registry (blocklist / HVCI) changed without reliable restore  
- Messaging implied unload/cleanup that kdmapper cannot do  
- Mapper path stopped Faceit / Vanguard-related services  

**After:**

- Defender exclusion narrowed to the **build directory only**, with remove-on-cleanup  
- Previous registry values **saved** and **restored** on clean exit (`dragonburn_host_state.txt` / mapper host-state file)  
- Honest messaging: mapped image stays until reboot  
- Mapper: `/restore-host` to undo prefs; no longer stops Faceit/Vanguard  
- Cleanup paths wired so restore isn’t double-called incorrectly  

**Files:**

- `dragonburn_usermode_final/memory/driver_manager.h`
- `dragonburn_kernelmode/main.cpp`
- `dragonburn_usermode_final/main.cpp` (cleanup / exit paths)

### 2.4 Offsets: `m_hObserverPawn`

**Before:** Field declared/used but never parsed → stayed `0`. Observer / map-change logic that depended on it was broken.

**After:**

- Parsed from a2x `client_dll.json` schema  
- Read as a `uint32` **CHandle**, not a raw pointer  
- Null controller checks hardened  

**Files:**

- `dragonburn_usermode_final/offsets.h`
- `dragonburn_usermode_final/entity_reader.h`

---

## 3. Medium-severity fixes

### Config save/load parity

Many menu toggles existed in `settings.h` / UI but were **not** written or read in `config.h` (e.g. trigger always-on, C4 ESP, bomb timer, scoped/flashed ESP, etc.). Those settings reset every launch.

**Fixed:** save/load coverage for the missing toggles (and later extended again for RCS / aimbot enrichments / world ESP).

**File:** `dragonburn_usermode_final/config.h`

### Memory backend actually wired

Settings had a memory-backend preference, but startup didn’t reliably use `CreateMemoryBackend` / the chosen backend.

**Fixed:** backend selection applies on launch (WinAPI / syscall / kernel driver).

**Files:** `main.cpp`, `menu.h`, settings/config

### Menu position persistence

Menu X/Y now persist through config instead of being lost.

### Keybind defaults

- Aimbot default: **Mouse 5** (`VK_XBUTTON2`)  
- Trigger default: **Mouse 4** (`VK_XBUTTON1`)  
(so they no longer collide on the same button)

### Config thrash / exit save

- Removed the ~250ms save thrash loop  
- Always save on clean exit (not only when the overlay was focused)  
- Save once when the menu closes  

### Attach / startup hardening

- Bounded waits for CS2 process / window (no endless hang)  
- Hard fail if `client.dll` base stays `0`  
- Safer null checks around controller / pawn paths  

### Docs / hygiene

- README build order and runtime notes rewritten  
- `*.sys` (and related build junk) covered in `.gitignore` so binaries aren’t committed  

---

## 4. Offsets: remote a2x dumps (smart cache)

**Before:** Relied on local / sidecar dump workflow (`cs2-dumper.exe` style), which was fragile and not really in the repo. Later briefly pulled from GitHub **every launch** (bad for rate limits while testing).

**After:**

- Downloads from a2x `output/offsets.json` + `client_dll.json` into `offsets/`
- **Smart cache** via `offsets/cache_meta.json`:
  - TTL **6 hours** (`kCacheMaxAgeHours`)
  - Also refreshes when on-disk **`client.dll` size/mtime** changes (CS2 update)
  - Also refreshes when live **`dwBuildNumber`** (engine2) differs from last run
  - Also refreshes if functional validation fails / cache corrupt
  - Otherwise reuses local JSON (safe for rapid relaunch testing)
- Temp download + rename; basic JSON sanity checks
- Linked `urlmon.lib` / `wininet.lib`

**Force refresh:** delete `offsets/cache_meta.json` or the cached JSON files.

**Also expanded schema parsing** for later features: aim punch services, shots fired, velocity, entity identity / designer name, grenade projectile trail fields, smoke, molotov, inferno, `dwGameEntitySystem_highestEntityIndex`, `engine2.dwBuildNumber`.

---

## 5. Feature work (only what was requested)

Explicitly **not** expanded into skins / bhop / OBS / other “cheat kitchen sink” ideas. Focus was:

### 5.1 RCS / recoil control

- Real Recoil panel (was “Coming soon”)  
- Standalone RCS using `CCSPlayer_AimPunchServices.m_predictableBaseAngle` + `m_iShotsFired`  
- Settings: enable, strength, start bullet, standalone (no aim key)  
- Avoids double-compensation when aimbot is holding key with “RCS Compensate”  
- Skips knives / nades / Zeus  
- Aimbot thread also starts when RCS alone is enabled  
- Config save/load  

**Files:** `aimbot.h`, `settings.h`, `config.h`, `menu.h`, `entity_reader.h`, `shared_state.h`, `main.cpp`

### 5.2 Richer aimbot

Added / wired:

- Multipoint (preferred bone + nearby bones)  
- Visibility check toggle (spotted mask + BVH when available)  
- Draw FOV circle  
- Velocity prediction + predict time  
- Humanize + strength  
- RCS compensate while aiming (aim through punch)  

**Files:** `aimbot.h`, `settings.h`, `config.h`, `menu.h`, `main.cpp` (`draw_aimbot_fov`)

### 5.3 World ESP — projectiles, trails, smoke, inferno

New `world_esp.h`:

- Scans entity list past player slots using highest-entity index  
- Classifies by `CEntityIdentity.m_designerName` (smoke/flash/HE/molly/decoy/inferno)  
- Draws projectile markers + distance labels  
- Reads `m_arrTrajectoryTrailPoints` (CUtlVector) for **trails**, with initial→current fallback  
- Smoke detonation marker when popped  
- Inferno / molotov fire points (`VectorWS` stride aware)  
- Menu: Misc → **World ESP** (toggles + colors + max distance)  
- Defaults / config persistence  

### 5.4 Grenade lineups

- Existing JSON lineup helper kept and emphasized in UI as **Grenade Lineups**  
- Enabled by default  
- Keybinds exposed in Settings (toggle / add / delete)  
- Still uses `grenades.json` (post-build copied next to usermode)  

**Files:** `grenades.h`, `menu.h`, `settings.h`, `main.cpp`

---

## 6. Build / runtime layout improvements

### Problem

README and loaders assumed you must manually place `dragonburn_driver.sys` and `DragonBurn-kernel.exe` **beside** the usermode executable after each build.

### Fix

- New resolver: `dragonburn_usermode_final/memory/artifact_paths.h`  
  - Finds repo root from the running exe  
  - Locates driver `.sys` and mapper `.exe` in **their own** build output folders  
  - Still accepts optional copies beside usermode for a portable pack  
- Usermode launches mapper as:  
  `DragonBurn-kernel.exe /wait "<absolute path to .sys>"`  
- Mapper accepts an explicit `.sys` path argument, or searches sibling build outputs  
- README updated: no mandatory “place next to usermode” step; offsets described as **every launch**  

**Files:**

- `memory/artifact_paths.h` (new)  
- `memory/driver_manager.h`  
- `memory/memory_driver.h`  
- `dragonburn_kernelmode/main.cpp`  
- `README.md`

---

## 7. Shared state / entity reading (supporting the above)

To feed RCS / prediction / aimbot:

- `AimbotFrame` gained punch, shots fired, local/target velocity  
- `LocalPlayerState` / `PlayerVisuals` carry punch, shots, velocity  
- Larger pawn snapshot window so `m_iShotsFired` / scoped fields fit  
- Main loop publishes those fields whenever aimbot, trigger, or RCS is enabled  

**Files:** `shared_state.h`, `entity_reader.h`, `types.h`, `main.cpp`

---

## Files touched most often (quick map)

| Component | Paths |
|-----------|--------|
| Usermode UI | `menu.h`, `ui/**`, `helpers/window_drag.*`, sidebar/topbar |
| Usermode combat | `aimbot.h`, `shared_state.h`, `settings.h`, `config.h` |
| Usermode world | `world_esp.h` (new), `grenades.h`, `main.cpp` |
| Offsets | `offsets.h` |
| Memory / host | `memory/driver_manager.h`, `memory/memory_driver.h`, `memory/shared.h`, `memory/artifact_paths.h` |
| Driver | `dragonburn_driver/.../driver.c` |
| Mapper | `dragonburn_kernelmode/main.cpp` |
| Docs | `README.md`, this file |

---

## What we deliberately did *not* do

- Did not turn this into a full feature clone of other cheats (skins, bhop, misc movement, OBS, etc.)  
- Did not claim mapped drivers can unload without reboot  
- Did not keep broad `%TEMP%` Defender exclusions or “stop anti-cheat services” behavior  
- Did not leave Recoil as a stub panel  

---

## How to verify after rebuild

1. Build **driver → mapper → usermode** (Release | x64).  
2. Run usermode as Admin with CS2 open — confirm console shows **fresh offset pull every launch**.  
3. Kernel backend should find `.sys` / mapper from build folders without manual copy.  
4. Menu: Combat → Aimbot / Recoil; Misc → World ESP + Grenade Lineups.  
5. Clean exit should restore tracked host registry prefs (another reboot may be needed for HVCI/blocklist to fully apply).  
