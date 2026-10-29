#pragma once

// Proxy-owned recovery: available even when the editor-side HTTP service is gone.
#include <tlhelp32.h>
#include <shellapi.h>
#include <filesystem>
#include <cwctype>
#include <cstdint>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace editor_recovery {
namespace fs = std::filesystem;
using json = nlohmann::json;

inline std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 path");
    std::wstring value(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), value.data(), size);
    return value;
}
inline std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string value(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), value.data(), size, nullptr, nullptr);
    return value;
}
inline std::wstring env(const wchar_t* key) {
    DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, 0);
    GetEnvironmentVariableW(key, value.data(), size);
    value.resize(size - 1);
    return value;
}
inline std::wstring normalized(const fs::path& path) {
    auto text = fs::weakly_canonical(fs::absolute(path)).wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return text;
}
inline std::wstring quote(const std::wstring& value) {
    // CommandLineToArgvW / CRT escaping, including trailing backslashes.
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
inline double epoch() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
inline json read_json(const fs::path& path) {
    std::ifstream input(path);
    if (!input) return json::object();
    json value; input >> value; return value;
}

struct Process {
    DWORD pid = 0;
    HANDLE handle = nullptr;
    fs::path executable;
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept : pid(other.pid), handle(other.handle), executable(other.executable) { other.handle = nullptr; }
    Process& operator=(Process&& other) noexcept {
        if (this != &other) { if (handle) CloseHandle(handle); pid = other.pid; handle = other.handle; executable = other.executable; other.handle = nullptr; }
        return *this;
    }
    ~Process() { if (handle) CloseHandle(handle); }
};

inline std::wstring command_line(HANDLE process) {
    using Query = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) return {};
    ULONG size = 0;
    query(process, 60 /* ProcessCommandLineInformation */, nullptr, 0, &size);
    if (!size || size > 1024 * 1024) return {};
    std::vector<unsigned char> storage(size);
    if (query(process, 60, storage.data(), size, &size) < 0) return {};
    struct UnicodeString { USHORT length, maximum; PWSTR buffer; };
    auto value = reinterpret_cast<UnicodeString*>(storage.data());
    auto begin = reinterpret_cast<uintptr_t>(storage.data());
    auto ptr = reinterpret_cast<uintptr_t>(value->buffer);
    if (ptr < begin || ptr + value->length > begin + storage.size()) return {};
    return std::wstring(value->buffer, value->length / sizeof(wchar_t));
}
inline bool matches_project(const std::wstring& command, const fs::path& project) {
    int count = 0;
    auto argv = CommandLineToArgvW(command.c_str(), &count);
    if (!argv) return false;
    bool matches = false;
    for (int i = 1; i < count; ++i) {
        fs::path arg(argv[i]);
        if (arg.is_absolute() && arg.extension() == L".uproject" && normalized(arg) == normalized(project)) matches = true;
    }
    LocalFree(argv); return matches;
}
inline std::vector<Process> find_editors(const fs::path& project, const fs::path& expected_executable) {
    std::vector<Process> matches;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate editor processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"UnrealEditor.exe")) continue;
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!handle) { CloseHandle(snapshot); throw std::runtime_error("Cannot inspect an UnrealEditor process; recovery will not guess its project"); }
        auto command = command_line(handle);
        if (command.empty()) { CloseHandle(handle); CloseHandle(snapshot); throw std::runtime_error("Cannot read an UnrealEditor command line; recovery will not launch a duplicate"); }
        if (!matches_project(command, project)) { CloseHandle(handle); continue; }
        wchar_t image[32768]; DWORD size = 32768;
        if (!QueryFullProcessImageNameW(handle, 0, image, &size)) { CloseHandle(handle); CloseHandle(snapshot); throw std::runtime_error("Cannot inspect editor executable"); }
        if (normalized(fs::path(std::wstring(image, size))) != normalized(expected_executable)) {
            CloseHandle(handle); CloseHandle(snapshot); throw std::runtime_error("This project is open in a different editor executable; inspect configuration before recovery");
        }
        Process process; process.pid = entry.th32ProcessID; process.handle = handle; process.executable = fs::path(std::wstring(image, size));
        matches.push_back(std::move(process));
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot); return matches;
}
inline bool restore_modal(DWORD pid) {
    struct Search { DWORD pid; bool found; } search{pid, false};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto search = reinterpret_cast<Search*>(data);
        DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
        if (owner == search->pid) {
            wchar_t title[512]{}; GetWindowTextW(window, title, 512);
            if (std::wstring(title).find(L"Restore Packages") != std::wstring::npos) search->found = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

class Manager {
    std::mutex lock;
    fs::path plugin, project, executable, state_file;
    bool automatic = false;
    std::string configuration_error, last_event = "not_observed";
    Process observed;
    double startup_until = 0;

    void configure() {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
        auto directory = fs::path(module).parent_path();
        // Both Binaries/monolith_proxy.exe and Tools/MonolithProxy builds are supported.
        for (auto root = directory; !root.empty() && root != root.parent_path(); root = root.parent_path()) {
            if (fs::exists(root / "Monolith.uplugin")) { plugin = root; break; }
        }
        auto explicit_plugin = env(L"MONOLITH_PLUGIN_DIR");
        if (!explicit_plugin.empty()) {
            plugin = fs::absolute(explicit_plugin);
            if (!fs::is_regular_file(plugin / "Monolith.uplugin")) throw std::runtime_error("MONOLITH_PLUGIN_DIR must contain Monolith.uplugin");
        }
        json config = plugin.empty() ? json::object() : read_json(plugin / "Config" / "EditorRecovery.json");
        automatic = config.value("auto_recover", false);
        auto flag = env(L"MONOLITH_AUTO_RECOVER_EDITOR");
        if (!flag.empty()) automatic = flag == L"1";
        auto configured_project = env(L"MONOLITH_PROJECT_FILE");
        if (configured_project.empty() && config.contains("project_file")) configured_project = wide(config.at("project_file").get<std::string>());
        if (!configured_project.empty()) project = fs::absolute(fs::path(configured_project).is_absolute() ? fs::path(configured_project) : plugin / configured_project);
        else if (!plugin.empty()) {
            for (auto root = plugin; root != root.parent_path(); root = root.parent_path()) {
                std::vector<fs::path> candidates;
                for (auto& item : fs::directory_iterator(root)) if (item.is_regular_file() && item.path().extension() == L".uproject") candidates.push_back(item.path());
                if (candidates.size() > 1) throw std::runtime_error("Multiple .uproject files; set MONOLITH_PROJECT_FILE");
                if (candidates.size() == 1) { project = candidates.front(); break; }
            }
        }
        if (project.empty() || project.extension() != L".uproject" || !fs::is_regular_file(project)) throw std::runtime_error("Set MONOLITH_PROJECT_FILE to an existing .uproject");
        auto configured_exe = env(L"MONOLITH_EDITOR_EXE");
        if (configured_exe.empty() && config.contains("editor_exe")) configured_exe = wide(config.at("editor_exe").get<std::string>());
        if (!configured_exe.empty()) executable = fs::absolute(fs::path(configured_exe).is_absolute() ? fs::path(configured_exe) : plugin / configured_exe);
        else {
            auto association = read_json(project).value("EngineAssociation", std::string());
            auto key = L"SOFTWARE\\EpicGames\\Unreal Engine\\" + wide(association);
            wchar_t installed[32768]{}; DWORD size = sizeof(installed);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"InstalledDirectory", RRF_RT_REG_SZ, nullptr, installed, &size) == ERROR_SUCCESS)
                executable = fs::path(installed) / "Engine/Binaries/Win64/UnrealEditor.exe";
            size = sizeof(installed);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Epic Games\\Unreal Engine\\Builds", wide(association).c_str(), RRF_RT_REG_SZ, nullptr, installed, &size) == ERROR_SUCCESS)
                executable = fs::path(installed) / "Engine/Binaries/Win64/UnrealEditor.exe";
            auto sibling = project.root_path() / (L"UE_" + wide(association)) / "Engine/Binaries/Win64/UnrealEditor.exe";
            if (executable.empty() && !association.empty() && fs::is_regular_file(sibling)) executable = sibling;
        }
        if (executable.empty() || !fs::is_regular_file(executable) || _wcsicmp(executable.filename().c_str(), L"UnrealEditor.exe")) throw std::runtime_error("Set MONOLITH_EDITOR_EXE to an existing UnrealEditor.exe");
        if (!plugin.empty()) state_file = plugin / "RuntimeData/EditorRecoveryState.json";
        else throw std::runtime_error("Cannot locate Monolith.uplugin; use a plugin-owned proxy executable");
    }
    std::wstring mutex_name() const {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : utf8(normalized(project))) { hash ^= c; hash *= 1099511628211ULL; }
        std::wostringstream text; text << L"Local\\MonolithEditorRecovery_" << std::hex << std::setfill(L'0') << std::setw(16) << hash; return text.str();
    }
    json snapshot() {
        json state{{"project_file", utf8(project.wstring())}, {"editor_exe", utf8(executable.wstring())}, {"auto_recover", automatic}, {"skip_restore_on_launch", true}, {"last_event", last_event}, {"configuration_error", configuration_error}};
        if (!configuration_error.empty()) { state["state"] = "configuration_error"; return state; }
        auto processes = find_editors(project, executable);
        state["editor_pids"] = json::array();
        bool modal = false;
        for (auto& process : processes) { state["editor_pids"].push_back(process.pid); modal = modal || restore_modal(process.pid); }
        state["state"] = processes.empty() ? "not_running" : modal ? "restore_prompt" : "running";
        state["startup_grace_active"] = epoch() < startup_until;
        return state;
    }
    json launch(bool is_auto, bool skip_restore) {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name().c_str());
        if (!mutex) throw std::runtime_error("Cannot acquire project recovery mutex");
        DWORD acquired = WaitForSingleObject(mutex, 0);
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) { CloseHandle(mutex); return {{"state", "recovery_in_progress"}}; }
        struct Guard { HANDLE value; ~Guard() { ReleaseMutex(value); CloseHandle(value); } } guard{mutex};
        auto previous = read_json(state_file);
        if (epoch() - previous.value("last_launch_unix", 0.0) < 300.0) { last_event = "cooldown"; return snapshot(); }
        auto processes = find_editors(project, executable);
        if (!processes.empty()) {
            if (processes.size() != 1 || is_auto || !skip_restore || !restore_modal(processes[0].pid)) return snapshot();
            // Validate durable state storage before stopping even a restore-modal editor.
            fs::create_directories(state_file.parent_path());
            std::ofstream preflight(state_file, std::ios::trunc);
            preflight << previous.dump(2); preflight.flush();
            if (!preflight) throw std::runtime_error("Cannot persist editor recovery state; editor left running");
            preflight.close();
            // Explicit recover may replace ONLY the exact-project editor stopped at this modal.
            HANDLE terminate = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processes[0].pid);
            if (!terminate) throw std::runtime_error("Cannot stop the identified Restore Packages process");
            if (!matches_project(command_line(terminate), project) || !restore_modal(processes[0].pid)) { CloseHandle(terminate); throw std::runtime_error("Restore process changed; inspect status again"); }
            if (!TerminateProcess(terminate, 0)) { CloseHandle(terminate); throw std::runtime_error("Cannot stop Restore Packages process"); }
            DWORD waited = WaitForSingleObject(terminate, 10000); CloseHandle(terminate);
            if (waited != WAIT_OBJECT_0) throw std::runtime_error("Restore process has not exited");
        }
        std::wstring command = quote(executable.wstring()) + L" " + quote(project.wstring()) + L" -AutoDeclinePackageRecovery";
        // Reserve cooldown durably BEFORE starting a process; a write failure must not
        // produce an untracked editor or let another proxy start a duplicate.
        fs::create_directories(state_file.parent_path());
        auto write_state = [&](const json& value) {
            std::ofstream state(state_file, std::ios::trunc);
            state << value.dump(2); state.flush();
            if (!state) throw std::runtime_error("Cannot persist editor recovery state");
        };
        write_state({{"last_launch_unix", epoch()}, {"pid", 0}});
        STARTUPINFOW info{}; info.cb = sizeof(info); info.dwFlags = STARTF_USESHOWWINDOW; info.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, project.parent_path().c_str(), &info, &child)) {
            auto error = GetLastError(); write_state(previous);
            throw std::runtime_error("CreateProcessW failed: " + std::to_string(error));
        }
        CloseHandle(child.hThread);
        observed = Process(); observed.pid = child.dwProcessId; observed.handle = child.hProcess; observed.executable = executable;
        startup_until = epoch() + 120.0; last_event = is_auto ? "crash_restart_started" : "recovery_started";
        json result{{"state", "starting"}, {"pid", child.dwProcessId}, {"project_file", utf8(project.wstring())}, {"editor_exe", utf8(executable.wstring())}, {"skip_restore", true}, {"automatic", is_auto}};
        try { write_state({{"last_launch_unix", epoch()}, {"pid", child.dwProcessId}}); }
        catch (const std::exception& error) { result["warning"] = error.what(); }
        return result;
    }
public:
    Manager() { try { configure(); } catch (const std::exception& error) { configuration_error = error.what(); } }
    json call(const json& args) {
        std::lock_guard<std::mutex> guard(lock);
        if (!args.is_object()) throw std::runtime_error("Recovery arguments must be an object");
        auto action = args.value("action", std::string("status"));
        if (action == "status") return snapshot();
        if (action != "recover") throw std::runtime_error("Expected action status or recover");
        if (!configuration_error.empty()) throw std::runtime_error(configuration_error);
        return launch(false, args.value("skip_restore", true));
    }
    void tick() {
        std::lock_guard<std::mutex> guard(lock);
        if (!automatic || !configuration_error.empty()) return;
        if (observed.handle) {
            DWORD code = STILL_ACTIVE;
            if (!GetExitCodeProcess(observed.handle, &code) || code == STILL_ACTIVE) return;
            bool failed_during_startup = epoch() < startup_until;
            observed = Process();
            if (code == 0) { last_event = "clean_exit"; return; }
            if (failed_during_startup) { last_event = "startup_failed_" + std::to_string(code); return; }
            last_event = "abnormal_exit_" + std::to_string(code);
            launch(true, true); return;
        }
        auto processes = find_editors(project, executable);
        if (processes.size() == 1) observed = std::move(processes[0]);
    }
};

inline Manager& manager() { static Manager instance; return instance; }
inline json tool() {
    return {{"name", "monolith_recovery"}, {"description", "Proxy-local Unreal Editor recovery, available while the editor is offline. status inspects this project's editor. recover launches it with -AutoDeclinePackageRecovery; an existing same-project Restore Packages modal can be replaced when skip_restore=true. Existing autosaves are preserved. Interrupted editor operations are never replayed."}, {"inputSchema", {{"type", "object"}, {"properties", {{"action", {{"type", "string"}, {"enum", {"status", "recover"}}, {"default", "status"}}}, {"skip_restore", {{"type", "boolean"}, {"default", true}}}}}, {"additionalProperties", false}}}};
}
inline void append_tool(json& tools) {
    for (auto& item : tools) if (item.is_object() && item.value("name", "") == "monolith_recovery") { item = tool(); return; }
    tools.push_back(tool());
}
} // namespace editor_recovery
