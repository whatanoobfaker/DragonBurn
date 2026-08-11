#include <Windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <fstream>

#include "kdmapper.h"
#include "utils.h"
#include "intel_driver.h"
#include "logger.h"

LONG WINAPI SimplestCrashHandler(EXCEPTION_POINTERS* ExceptionInfo);
bool CheckWindowsKernelPrefs();
bool ApplyRecommendedPrefs();
void RestoreRecommendedPrefs();
std::wstring HostStatePath();

int wmain()
{
	SetUnhandledExceptionFilter(SimplestCrashHandler);

	bool waitForKey = false;
	for (int i = 1; i < __argc; i++) {
		if (wcscmp(__wargv[i], L"/wait") == 0 || wcscmp(__wargv[i], L"/pause") == 0) {
			waitForKey = true;
			break;
		}
		if (wcscmp(__wargv[i], L"/restore-host") == 0) {
			RestoreRecommendedPrefs();
			return 0;
		}
	}

	Log::Info("dragonburn_driver mapper using Intel vulnerable driver");

	HANDLE hDevice = CreateFileW(L"\\\\.\\dragonburn_driver", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hDevice != INVALID_HANDLE_VALUE) {
		CloseHandle(hDevice);
		Log::Custom("[+] dragonburn_driver already loaded", 10);
		return 0;
	}

	/* Check and apply kernel prefs — do NOT stop third-party services */
	if (!CheckWindowsKernelPrefs())
	{
		Log::Warning("Some kernel preferences may interfere with driver loading.");
		std::string response;
		do
		{
			Log::Info("Apply recommended settings (saved for restore) and reboot? (y/n)");
			std::cout << ">>> "; std::cin >> response;
		} while (response != "y" && response != "n");
		if (response == "y")
		{
			if (!ApplyRecommendedPrefs()) {
				Log::Error("Failed to apply recommended settings", false);
				return -1;
			}
			Log::Fine("Settings applied and saved. Reboot required.");
			Log::Info("After you are done, run: DragonBurn-kernel.exe /restore-host");
			system("pause");
			return -1;
		}
	}

	std::wstring driver_path = kdmUtils::GetCurrentAppFolder() + L"\\dragonburn_driver.sys";
	if (!std::filesystem::exists(driver_path)) {
		Log::Error("dragonburn_driver.sys not found next to this executable");
	}

	std::vector<uint8_t> driver_data;
	if (!kdmUtils::ReadFileToMemory(driver_path, &driver_data) || driver_data.empty()) {
		Log::Error("Failed to read dragonburn_driver.sys");
	}

	Log::Fine("Read " + std::to_string(driver_data.size()) + " bytes from dragonburn_driver.sys");

	if (!NT_SUCCESS(intel_driver::Load()))
		Log::Error("Failed to connect to intel driver");

	kdmapper::AllocationMode mode = kdmapper::AllocationMode::AllocatePool;

	NTSTATUS exitCode = 0;
	if (!kdmapper::MapDriver(driver_data.data(), 0, 0, false, true, mode, false, nullptr, &exitCode))
	{
		intel_driver::Unload();
		Log::Error("Failed to map dragonburn_driver.sys");
	}

	if (!NT_SUCCESS(intel_driver::Unload()))
		Log::Warning("Warning failed to unload intel driver", true);

	Log::Fine("dragonburn_driver.sys mapped successfully");
	Log::Info("Mapped drivers stay until reboot. Use /restore-host later to undo registry prefs.");

	if (waitForKey) {
		Log::Custom("--- Mapping complete. Press any key to close this window ---", 8);
		system("pause>nul");
	}

	return 0;
}

std::wstring HostStatePath()
{
	return kdmUtils::GetCurrentAppFolder() + L"\\dragonburn_mapper_host_state.txt";
}

static bool QueryDword(HKEY root, const char* subkey, const char* name, DWORD* out, DWORD def)
{
	*out = def;
	HKEY hKey = nullptr;
	if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return false;
	DWORD size = sizeof(DWORD), type = 0;
	LONG st = RegQueryValueExA(hKey, name, nullptr, &type, (LPBYTE)out, &size);
	RegCloseKey(hKey);
	return st == ERROR_SUCCESS && type == REG_DWORD;
}

bool CheckWindowsKernelPrefs()
{
	/* Vulnerable Driver Blocklist */
	{
		DWORD data = 0;
		if (QueryDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
			"VulnerableDriverBlocklistEnable", &data, 0) && data != 0)
			return false;
	}

	/* HypervisorEnforcedCodeIntegrity (HVCI) */
	{
		DWORD data = 0;
		if (QueryDword(HKEY_LOCAL_MACHINE,
			"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
			"Enabled", &data, 0) && data != 0)
			return false;
	}

	return true;
}

bool ApplyRecommendedPrefs()
{
	Log::Info("Saving current kernel preferences, then applying recommended values...");

	DWORD prev_blocklist = 1, prev_hvci = 1, prev_ppl = 0, prev_vbs = 0;
	QueryDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
		"VulnerableDriverBlocklistEnable", &prev_blocklist, 1);
	QueryDword(HKEY_LOCAL_MACHINE,
		"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
		"Enabled", &prev_hvci, 1);
	QueryDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Lsa",
		"RunAsPPL", &prev_ppl, 0);
	QueryDword(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Control\\DeviceGuard",
		"EnableVirtualizationBasedSecurity", &prev_vbs, 0);

	{
		std::wofstream out(HostStatePath());
		if (!out) {
			Log::Error("Could not write host state file", false);
			return false;
		}
		out << L"prev_blocklist=" << prev_blocklist << L"\n";
		out << L"prev_hvci=" << prev_hvci << L"\n";
		out << L"prev_ppl=" << prev_ppl << L"\n";
		out << L"prev_vbs=" << prev_vbs << L"\n";
		out << L"changed=1\n";
	}

	system("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity\" /v Enabled /t REG_DWORD /d 0 /f >nul 2>&1");
	system("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Lsa\" /v RunAsPPL /t REG_DWORD /d 0 /f >nul 2>&1");
	system("reg add \"HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Control\\DeviceGuard\" /v EnableVirtualizationBasedSecurity /t REG_DWORD /d 00000000 /f >nul 2>&1");
	system("bcdedit /set hypervisorlaunchtype off >nul 2>&1");
	system("reg add \"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\CI\\Config\" /v VulnerableDriverBlocklistEnable /t REG_DWORD /d 00000000 /f >nul 2>&1");
	Log::Fine("Done. Original values saved to dragonburn_mapper_host_state.txt");
	return true;
}

void RestoreRecommendedPrefs()
{
	std::wifstream in(HostStatePath());
	if (!in) {
		Log::Warning("No mapper host state file found — nothing to restore", true);
		return;
	}

	DWORD prev_blocklist = 1, prev_hvci = 1, prev_ppl = 0, prev_vbs = 0;
	bool changed = false;
	std::wstring line;
	while (std::getline(in, line)) {
		if (line.rfind(L"prev_blocklist=", 0) == 0) prev_blocklist = (DWORD)_wtoi(line.c_str() + 15);
		else if (line.rfind(L"prev_hvci=", 0) == 0) prev_hvci = (DWORD)_wtoi(line.c_str() + 10);
		else if (line.rfind(L"prev_ppl=", 0) == 0) prev_ppl = (DWORD)_wtoi(line.c_str() + 9);
		else if (line.rfind(L"prev_vbs=", 0) == 0) prev_vbs = (DWORD)_wtoi(line.c_str() + 9);
		else if (line.rfind(L"changed=", 0) == 0) changed = _wtoi(line.c_str() + 8) != 0;
	}
	in.close();

	if (!changed) {
		Log::Info("Host state present but not marked changed.");
		return;
	}

	Log::Info("Restoring previous kernel preferences...");

	auto set_dword = [](const char* key, const char* name, DWORD value) {
		char cmd[512];
		sprintf_s(cmd, "reg add \"%s\" /v %s /t REG_DWORD /d %lu /f >nul 2>&1", key, name, value);
		system(cmd);
	};

	set_dword("HKLM\\SYSTEM\\CurrentControlSet\\Control\\CI\\Config", "VulnerableDriverBlocklistEnable", prev_blocklist);
	set_dword("HKLM\\SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Enabled", prev_hvci);
	set_dword("HKLM\\SYSTEM\\CurrentControlSet\\Control\\Lsa", "RunAsPPL", prev_ppl);
	set_dword("HKLM\\System\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", prev_vbs);

	DeleteFileW(HostStatePath().c_str());
	Log::Fine("Restored. Reboot for registry/BCD changes to take full effect.");
}

LONG WINAPI SimplestCrashHandler(EXCEPTION_POINTERS* ExceptionInfo)
{
	if (ExceptionInfo && ExceptionInfo->ExceptionRecord)
	{
		std::ostringstream oss;
		oss << "Crash at addr 0x" << ExceptionInfo->ExceptionRecord->ExceptionAddress << L" by 0x" << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode;
		Log::Error(oss.str(), false);
	}
	else
		Log::Error("Program crashed", false);

	if (intel_driver::hDevice)
		intel_driver::Unload();

	return EXCEPTION_EXECUTE_HANDLER;
}
