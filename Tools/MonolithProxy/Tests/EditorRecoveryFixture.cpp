// Isolated process fixture. Build as UnrealEditor.exe inside a temporary test tree.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <string>

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--fixture-crash") { Sleep(6000); return 42; }
        if (std::wstring(argv[i]) == L"--fixture-clean") { Sleep(6000); return 0; }
    }
    wchar_t destination[32768]{};
    GetEnvironmentVariableW(L"MONOLITH_FIXTURE_LAUNCH_LOG", destination, 32768);
    if (*destination) {
        std::ofstream log{std::filesystem::path(destination), std::ios::app};
        log << GetCurrentProcessId();
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"-AutoDeclinePackageRecovery") log << " skip_restore";
        }
        log << '\n'; log.flush();
    }
    Sleep(30000);
    return 0;
}
