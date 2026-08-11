# DragonBurn

Windows usermode overlay + optional kernel memory driver for CS2. Three projects; build order matters.

**What we changed in the recent hardening/feature pass:** see [`CHANGES.md`](CHANGES.md).

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 (MSVC / WDK for the driver)
- Administrator privileges for the kernel/kdmapper path
- CS2 running
- Internet access **on every launch** (offsets are pulled fresh from [a2x/cs2-dumper](https://github.com/a2x/cs2-dumper) `output/` each run; local files are only a fallback if GitHub is unreachable)

## Build order

1. **Driver** — open `dragonburn_driver/dragonburn_driver.slnx` (or the `.vcxproj`), build **Release | x64**  
   Output: `dragonburn_driver.sys` (typically under `dragonburn_driver/x64/Release/` or `dragonburn_driver/dragonburn_driver/x64/Release/`)

2. **Mapper** — open `dragonburn_kernelmode/DragonBurn-kernel.slnx`, build **Release | x64**  
   Output: `dragonburn_kernelmode/built/DragonBurn-kernel.exe`

3. **Usermode** — open `dragonburn_usermode_final/Dragonburn-user.sln`, build **Release | x64**  
   Output: `dragonburn_usermode_final/build/Release/` (icons + `grenades.json` are post-build copied)

You do **not** need to copy the driver or mapper next to the usermode exe. When the kernel backend starts, usermode resolves those artifacts from their own build folders (or from an optional path you pass to the mapper). Leaving copies beside the usermode exe still works if you prefer a portable pack.

## Offsets

Every launch downloads:

- `https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json`
- `https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json`

into `offsets/` next to the usermode executable. Cached copies are used only if the download fails.

## Run

1. Start CS2  
2. Run the usermode exe **as Administrator** (kernel backend)  
3. First run may ask to change Vulnerable Driver Blocklist / HVCI and reboot — previous values are saved and restored on clean exit  
4. Menu default: `F1` · Master: `F2` · Exit: `Insert`  
5. Aimbot default bind: Mouse 5 · Trigger: Mouse 4  

Restore mapper registry prefs manually if needed:

```text
DragonBurn-kernel.exe /restore-host
```

You can also map manually with an explicit driver path:

```text
DragonBurn-kernel.exe /wait "C:\path\to\dragonburn_driver.sys"
```

## Memory backends

Configurable in **Settings → General → Memory Backend** (applies on next launch):

1. WinAPI  
2. Indirect syscall  
3. Kernel driver via kdmapper (default)

## Notes

- Mapped drivers are not unloadable without a reboot; host Defender/registry changes are tracked in `dragonburn_host_state.txt` / `dragonburn_mapper_host_state.txt`  
- Do not commit `.sys`, `.exe`, or host-state files (see `.gitignore`)
- If a2x's dump lags a brand-new CS2 patch, wait for their repo to update (or ESP will fail the functional test)
