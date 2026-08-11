#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>
#include <vector>

// Resolve driver/mapper artifacts from either:
// 1) next to the current exe (optional portable layout), or
// 2) their own project build outputs under the repo root.
// No manual copy between projects is required after a normal build.
namespace DragonBurnPaths {

inline std::filesystem::path exe_directory() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

inline std::filesystem::path find_repo_root() {
    std::filesystem::path cur = exe_directory();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(cur / "dragonburn_usermode_final") &&
            std::filesystem::exists(cur / "dragonburn_kernelmode") &&
            std::filesystem::exists(cur / "dragonburn_driver"))
        {
            return cur;
        }
        if (!cur.has_parent_path() || cur == cur.root_path())
            break;
        cur = cur.parent_path();
    }
    return {};
}

inline std::filesystem::path first_existing(const std::vector<std::filesystem::path>& cands) {
    for (const auto& p : cands) {
        std::error_code ec;
        if (!p.empty() && std::filesystem::exists(p, ec) && !ec)
            return std::filesystem::weakly_canonical(p, ec);
    }
    return {};
}

inline std::filesystem::path find_driver_sys() {
    const auto exe = exe_directory();
    const auto root = find_repo_root();

    std::vector<std::filesystem::path> cands;
    cands.push_back(exe / L"dragonburn_driver.sys");

    if (!root.empty()) {
        cands.push_back(root / L"dragonburn_driver" / L"x64" / L"Release" / L"dragonburn_driver.sys");
        cands.push_back(root / L"dragonburn_driver" / L"x64" / L"Debug" / L"dragonburn_driver.sys");
        cands.push_back(root / L"dragonburn_driver" / L"dragonburn_driver" / L"x64" / L"Release" / L"dragonburn_driver.sys");
        cands.push_back(root / L"dragonburn_driver" / L"dragonburn_driver" / L"x64" / L"Debug" / L"dragonburn_driver.sys");
        // Some WDK layouts drop under solutiondir\Release
        cands.push_back(root / L"dragonburn_driver" / L"Release" / L"dragonburn_driver.sys");
        cands.push_back(root / L"dragonburn_driver" / L"Debug" / L"dragonburn_driver.sys");
    }

    // Relative fallbacks when launched from a build folder without full repo markers
    cands.push_back(exe / L".." / L".." / L".." / L"dragonburn_driver" / L"x64" / L"Release" / L"dragonburn_driver.sys");
    cands.push_back(exe / L".." / L".." / L".." / L"dragonburn_driver" / L"dragonburn_driver" / L"x64" / L"Release" / L"dragonburn_driver.sys");

    return first_existing(cands);
}

inline std::filesystem::path find_kdmapper_exe() {
    const auto exe = exe_directory();
    const auto root = find_repo_root();

    std::vector<std::filesystem::path> cands;
    cands.push_back(exe / L"DragonBurn-kernel.exe");

    if (!root.empty()) {
        cands.push_back(root / L"dragonburn_kernelmode" / L"built" / L"DragonBurn-kernel.exe");
        cands.push_back(root / L"dragonburn_kernelmode" / L"built_dbg" / L"DragonBurn-kernel.exe");
    }

    cands.push_back(exe / L".." / L".." / L".." / L"dragonburn_kernelmode" / L"built" / L"DragonBurn-kernel.exe");
    cands.push_back(exe / L".." / L".." / L".." / L"dragonburn_kernelmode" / L"built_dbg" / L"DragonBurn-kernel.exe");

    return first_existing(cands);
}

} // namespace DragonBurnPaths
