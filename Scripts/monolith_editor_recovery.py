"""Windows proxy-owned editor recovery. Never retries editor requests."""
import ctypes
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import subprocess
import threading
import time

PLUGIN_DIR = Path(os.environ.get("MONOLITH_PLUGIN_DIR", str(Path(__file__).resolve().parent.parent))).resolve()

def validate_plugin():
    if not (PLUGIN_DIR / "Monolith.uplugin").is_file():
        raise ValueError("MONOLITH_PLUGIN_DIR must contain Monolith.uplugin")

def configuration():
    validate_plugin()
    path = PLUGIN_DIR / "Config/EditorRecovery.json"
    return json.loads(path.read_text(encoding="utf-8-sig")) if path.is_file() else {}

def configured_path(value):
    path = Path(value)
    return str(path if path.is_absolute() else PLUGIN_DIR / path)

def auto_enabled():
    return os.environ.get("MONOLITH_AUTO_RECOVER_EDITOR", "1" if configuration().get("auto_recover", False) else "0") == "1"

def project_hash(project):
    value = 14695981039346656037
    for byte in normalized(project).lower().encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def normalized(path):
    return os.path.normcase(os.path.realpath(os.path.abspath(str(path))))


def resolve_paths():
    config = configuration()
    project = os.environ.get("MONOLITH_PROJECT_FILE") or (configured_path(config["project_file"]) if config.get("project_file") else None)
    if not project:
        for root in [PLUGIN_DIR, *PLUGIN_DIR.parents]:
            candidates = list(root.glob("*.uproject"))
            if len(candidates) > 1:
                raise ValueError("Set MONOLITH_PROJECT_FILE: plugin ancestry contains multiple .uproject files")
            if candidates:
                project = str(candidates[0]); break
        if not project:
            raise ValueError("Set MONOLITH_PROJECT_FILE: no ancestor .uproject found")
    project = Path(project).resolve()
    if project.suffix.lower() != ".uproject" or not project.is_file():
        raise ValueError("MONOLITH_PROJECT_FILE must identify an existing .uproject")
    exe = os.environ.get("MONOLITH_EDITOR_EXE") or (configured_path(config["editor_exe"]) if config.get("editor_exe") else None)
    if not exe:
        import winreg
        association = json.loads(project.read_text(encoding="utf-8-sig")).get("EngineAssociation", "")
        roots = []
        for hive, key, value in [
            (winreg.HKEY_CURRENT_USER, r"Software\Epic Games\Unreal Engine\Builds", association),
            (winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\EpicGames\Unreal Engine\{association}", "InstalledDirectory"),
        ]:
            try:
                with winreg.OpenKey(hive, key) as handle:
                    roots.append(Path(winreg.QueryValueEx(handle, value)[0]))
            except OSError:
                pass
        roots.extend([project.parent, Path(project.anchor) / ("UE_" + association)])
        for root in roots:
            candidate = root / "Engine/Binaries/Win64/UnrealEditor.exe"
            if candidate.is_file():
                exe = str(candidate)
                break
    if not exe or not Path(exe).is_file() or Path(exe).name.lower() != "unrealeditor.exe":
        raise ValueError("Set MONOLITH_EDITOR_EXE to an existing UnrealEditor.exe")
    return str(project), str(Path(exe).resolve())


class NativeWindows:
    """Retain a process handle so PID reuse cannot change the watched identity."""
    def __init__(self):
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.n = ctypes.WinDLL("ntdll")
        self.s = ctypes.WinDLL("shell32")
        self.u = ctypes.WinDLL("user32", use_last_error=True)
        self.k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.k.OpenProcess.restype = W.HANDLE
        self.k.CloseHandle.argtypes = [W.HANDLE]
        self.k.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
        self.k.CreateToolhelp32Snapshot.restype = W.HANDLE
        self.k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, ctypes.POINTER(W.DWORD)]
        self.k.GetExitCodeProcess.argtypes = [W.HANDLE, ctypes.POINTER(W.DWORD)]
        self.k.GetProcessTimes.argtypes = [W.HANDLE] + [ctypes.POINTER(W.FILETIME)] * 4
        self.k.CreateMutexW.argtypes = [W.LPVOID, W.BOOL, W.LPCWSTR]
        self.k.CreateMutexW.restype = W.HANDLE
        self.k.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
        self.k.ReleaseMutex.argtypes = [W.HANDLE]
        self.k.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
        self.k.LocalFree.argtypes = [W.HANDLE]
        self.s.CommandLineToArgvW.argtypes = [W.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
        self.s.CommandLineToArgvW.restype = ctypes.POINTER(W.LPWSTR)
        self.n.NtQueryInformationProcess.argtypes = [W.HANDLE, ctypes.c_ulong, W.LPVOID, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]

    def close(self, handle):
        self.k.CloseHandle(handle)

    def exit_code(self, handle):
        code = W.DWORD()
        if not self.k.GetExitCodeProcess(handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return None if code.value == 259 else code.value

    def processes(self, project, exe):
        class Entry(ctypes.Structure):
            _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("pid", W.DWORD),
                        ("heap", ctypes.c_size_t), ("module", W.DWORD), ("threads", W.DWORD),
                        ("parent", W.DWORD), ("priority", W.LONG), ("flags", W.DWORD), ("name", W.WCHAR * 260)]
        class UnicodeString(ctypes.Structure):
            _fields_ = [("length", W.USHORT), ("maximum", W.USHORT), ("buffer", ctypes.c_void_p)]
        self.k.Process32FirstW.argtypes = [W.HANDLE, ctypes.POINTER(Entry)]
        self.k.Process32NextW.argtypes = [W.HANDLE, ctypes.POINTER(Entry)]
        snap = self.k.CreateToolhelp32Snapshot(2, 0)
        if snap == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        found = []
        entry = Entry(); entry.size = ctypes.sizeof(entry)
        try:
            valid = self.k.Process32FirstW(snap, ctypes.byref(entry))
            while valid:
                if entry.name.lower() == "unrealeditor.exe":
                    h = self.k.OpenProcess(0x1000 | 0x00100000 | 1, False, entry.pid)
                    if not h:
                        raise PermissionError(f"Cannot inspect editor PID {entry.pid}; refusing duplicate launch")
                    keep = False
                    try:
                        image = ctypes.create_unicode_buffer(32768); size = W.DWORD(len(image))
                        if not self.k.QueryFullProcessImageNameW(h, 0, image, ctypes.byref(size)):
                            raise ctypes.WinError(ctypes.get_last_error())
                        needed = ctypes.c_ulong()
                        self.n.NtQueryInformationProcess(h, 60, None, 0, ctypes.byref(needed))
                        buf = ctypes.create_string_buffer(max(needed.value, 65536))
                        if self.n.NtQueryInformationProcess(h, 60, buf, len(buf), ctypes.byref(needed)) < 0:
                            raise RuntimeError(f"Cannot read editor PID {entry.pid} command line")
                        us = UnicodeString.from_buffer(buf)
                        command = ctypes.wstring_at(us.buffer, us.length // 2)
                        count = ctypes.c_int(); argv = self.s.CommandLineToArgvW(command, ctypes.byref(count))
                        if not argv:
                            raise ctypes.WinError(ctypes.get_last_error())
                        try:
                            matches = any(normalized(argv[i]) == normalized(project) for i in range(1, count.value))
                        finally:
                            self.k.LocalFree(ctypes.cast(argv, W.HANDLE))
                        if matches:
                            if normalized(image.value) != normalized(exe):
                                raise RuntimeError("Same project is running under a different editor executable")
                            times = [W.FILETIME() for _ in range(4)]
                            if not self.k.GetProcessTimes(h, *(ctypes.byref(t) for t in times)):
                                raise ctypes.WinError(ctypes.get_last_error())
                            creation = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
                            found.append({"pid": entry.pid, "handle": h, "creation_time": creation})
                            keep = True
                    finally:
                        if not keep: self.close(h)
                valid = self.k.Process32NextW(snap, ctypes.byref(entry))
        except Exception:
            for item in found: self.close(item["handle"])
            raise
        finally:
            self.close(snap)
        return found

    def restore_modal(self, pid):
        titles = []
        callback_type = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        self.u.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]
        self.u.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, ctypes.c_int]
        self.u.IsWindowVisible.argtypes = [W.HWND]
        @callback_type
        def visit(hwnd, unused):
            owner = W.DWORD(); self.u.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and self.u.IsWindowVisible(hwnd):
                title = ctypes.create_unicode_buffer(1024); self.u.GetWindowTextW(hwnd, title, len(title))
                if "restore packages" in title.value.lower(): titles.append(title.value)
            return True
        self.u.EnumWindows.argtypes = [callback_type, W.LPARAM]
        self.u.EnumWindows(visit, 0)
        return titles

    def launch_locked(self, project, exe, terminate=None):
        name = "Local\\MonolithEditorRecovery_" + project_hash(project)
        mutex = self.k.CreateMutexW(None, False, name)
        if not mutex: raise ctypes.WinError(ctypes.get_last_error())
        acquired = self.k.WaitForSingleObject(mutex, 0) in (0, 0x80)
        try:
            if not acquired: return {"launched": False, "reason": "another_proxy_is_recovering"}
            current = self.processes(project, exe)
            try:
                state_file = PLUGIN_DIR / "RuntimeData/EditorRecoveryState.json"
                if state_file.is_file():
                    previous = json.loads(state_file.read_text(encoding="utf-8"))
                    if time.time() - float(previous.get("last_launch_unix", 0)) < 300:
                        return {"launched": False, "reason": "shared_launch_cooldown"}
                state_file.parent.mkdir(parents=True, exist_ok=True)
                # Reserve cooldown before any process mutation; errors leave editors untouched.
                if not current or terminate:
                    state_file.write_text(json.dumps({"last_launch_unix": time.time()}), encoding="utf-8")
                if terminate:
                    matching = [p for p in current if p["pid"] == terminate["pid"] and p["creation_time"] == terminate["creation_time"]]
                    if len(current) != 1 or not matching or not self.restore_modal(matching[0]["pid"]):
                        return {"launched": False, "reason": "restore_modal_identity_changed"}
                    if not self.k.TerminateProcess(matching[0]["handle"], 1): raise ctypes.WinError(ctypes.get_last_error())
                    if self.k.WaitForSingleObject(matching[0]["handle"], 10000) != 0:
                        raise RuntimeError("Restore Packages editor did not exit")
                elif current:
                    return {"launched": False, "reason": "same_project_editor_running", "pid": current[0]["pid"]}
                startup = subprocess.STARTUPINFO(); startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 1
                process = subprocess.Popen([exe, project, "-AutoDeclinePackageRecovery"], cwd=str(Path(project).parent), shell=False, startupinfo=startup,
                                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return {"launched": True, "pid": process.pid, "restore_packages_skipped": True}
            finally:
                for item in current: self.close(item["handle"])
        finally:
            if acquired: self.k.ReleaseMutex(mutex)
            self.close(mutex)


class EditorRecovery:
    def __init__(self, native=None, resolver=resolve_paths, clock=time.monotonic):
        self.native = native
        self.resolver = resolver
        self.clock = clock
        self.lock = threading.RLock()
        self.watched = None
        self.last_auto = None
        self.launched_at = None
        self.last_result = None

    def _native(self):
        if self.native is None:
            if os.name != "nt": raise RuntimeError("Editor recovery supports Windows only")
            self.native = NativeWindows()
        return self.native

    def _observe(self, project, exe):
        native = self._native()
        exited = None
        if self.watched:
            exited = native.exit_code(self.watched["handle"])
            if exited is not None:
                native.close(self.watched["handle"]); self.watched = None
        current = native.processes(project, exe)
        for item in current:
            if not self.watched:
                self.watched = item
            else:
                native.close(item["handle"])
        return current, exited

    def status(self, healthy=False):
        with self.lock:
            try:
                project, exe = self.resolver()
                current, exited = self._observe(project, exe)
                return {"ok": True, "project_file": project, "editor_exe": exe,
                        "pid": self.watched["pid"] if self.watched else None,
                        "process_count": len(current), "editor_healthy": healthy,
                        "auto_recovery_enabled": auto_enabled(),
                        "restore_packages_modal": bool(self.watched and self._native().restore_modal(self.watched["pid"])),
                        "last_observed_exit_code": exited, "last_recovery": self.last_result,
                        "startup_grace_active": self.launched_at is not None and self.clock() - self.launched_at < 120}
            except Exception as error:
                return {"ok": False, "error": str(error), "editor_healthy": healthy}

    def recover(self, healthy=False, skip_restore_packages=True):
        with self.lock:
            state = self.status(healthy)
            if not state["ok"]: return state
            if healthy: return dict(state, launched=False, reason="editor_healthy")
            if state["startup_grace_active"]: return dict(state, launched=False, reason="startup_grace")
            terminate = self.watched if state["process_count"] == 1 and state["restore_packages_modal"] and skip_restore_packages else None
            if state["process_count"] and terminate is None:
                return dict(state, launched=False, reason="same_project_editor_running")
            try:
                result = self._native().launch_locked(state["project_file"], state["editor_exe"], terminate)
                if result.get("launched"): self.launched_at = self.clock()
                self.last_result = result
                return dict(state, **result)
            except Exception as error:
                return dict(state, ok=False, error=str(error))

    def tick(self, healthy=False):
        with self.lock:
            state = self.status(healthy)
            if (not state.get("ok") or not state.get("auto_recovery_enabled") or healthy
                    or state.get("pid") or state.get("startup_grace_active")
                    or state.get("last_observed_exit_code") in (None, 0)
                    or (self.last_auto is not None and self.clock() - self.last_auto < 300)):
                return state
            self.last_auto = self.clock()
            return self.recover(False, False)


RECOVERY = EditorRecovery()


def tool_schema():
    return {"name": "monolith_recovery", "description": "Proxy-local Windows editor status and same-project recovery; available while Unreal Editor is offline. Recovery preserves autosaves and never replays editor mutations.",
            "inputSchema": {"type": "object", "properties": {"action": {"type": "string", "enum": ["status", "recover"]},
                "skip_restore": {"type": "boolean", "default": True}}, "required": ["action"], "additionalProperties": False}}


def append_tool(tools):
    return [item for item in tools if item.get("name") != "monolith_recovery"] + [tool_schema()]
