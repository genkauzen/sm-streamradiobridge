#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <string>

enum class BridgeState { None, SameDll, DifferentDll };

static DWORD FindGame() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ScrapMechanic.exe") == 0) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

static std::wstring ModulePath() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

static BridgeState FindLoadedBridge(DWORD pid, const std::wstring& requestedDll,
                                    std::wstring& loadedPath) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return BridgeState::None;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BridgeState result = BridgeState::None;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            std::wstring name(entry.szModule);
            if (name.size() >= 17 &&
                _wcsnicmp(name.c_str(), L"StreamRadioBridge", 17) == 0) {
                loadedPath = entry.szExePath;
                result = _wcsicmp(loadedPath.c_str(), requestedDll.c_str()) == 0
                    ? BridgeState::SameDll : BridgeState::DifferentDll;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

static bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) return false;

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return false;
    }
    bool ok = WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr);
    HANDLE thread = nullptr;
    if (ok) {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        ok = thread != nullptr;
    }
    if (thread) {
        const DWORD waitResult = WaitForSingleObject(thread, 10000);
        DWORD remoteResult = 0;
        if (waitResult != WAIT_OBJECT_0 ||
            !GetExitCodeThread(thread, &remoteResult) || remoteResult == 0) {
            ok = false;
        }
        CloseHandle(thread);
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring requested = argc > 1 ? argv[1] : ModulePath() + L"\\StreamRadioBridge.dll";
    wchar_t absolutePath[32768]{};
    const DWORD pathLength = GetFullPathNameW(requested.c_str(), 32768, absolutePath, nullptr);
    if (pathLength == 0 || pathLength >= 32768 ||
        GetFileAttributesW(absolutePath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"StreamRadioBridge injector\n");
        wprintf(L"DLL was not found: %ls\n", requested.c_str());
        return 1;
    }
    const std::wstring dll(absolutePath);
    wprintf(L"StreamRadioBridge injector\n");
    wprintf(L"DLL: %ls\n", dll.c_str());

    DWORD pid = 0;
    for (int i = 0; i < 60 && !pid; ++i) {
        pid = FindGame();
        if (!pid) Sleep(1000);
    }
    if (!pid) {
        wprintf(L"ScrapMechanic.exe was not found. Start the game first.\n");
        return 2;
    }

    std::wstring loadedBridge;
    const BridgeState bridgeState = FindLoadedBridge(pid, dll, loadedBridge);
    if (bridgeState == BridgeState::SameDll) {
        wprintf(L"Bridge is already loaded in ScrapMechanic.exe (%lu).\n", pid);
        return 0;
    }
    if (bridgeState == BridgeState::DifferentDll) {
        wprintf(L"Another StreamRadioBridge is already loaded:\n%ls\n", loadedBridge.c_str());
        wprintf(L"Fully close Scrap Mechanic before installing or changing the DLL.\n");
        return 4;
    }
    if (!Inject(pid, dll)) {
        wprintf(L"Injection failed, Win32=%lu\n", GetLastError());
        return 3;
    }
    wprintf(L"Injected into ScrapMechanic.exe (%lu).\n", pid);
    return 0;
}
