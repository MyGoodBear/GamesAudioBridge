#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <filesystem>
#include <cstring>
#include <cctype>


using ALchar = char;
using ALenum = int;
using ALboolean = char;
struct ProbeALCdevice;
struct ProbeALCcontext;
using ProbeOpenDevice = ProbeALCdevice* (__cdecl*)(const ALchar*);
using ProbeCreateContext = ProbeALCcontext* (__cdecl*)(ProbeALCdevice*, const int*);
using ProbeMakeCurrent = ALboolean (__cdecl*)(ProbeALCcontext*);
using ProbeDestroyContext = void (__cdecl*)(ProbeALCcontext*);
using ProbeCloseDevice = ALboolean (__cdecl*)(ProbeALCdevice*);
using ProbeGetString = const ALchar* (__cdecl*)(ALenum);
using ProbeCGetString = const ALchar* (__cdecl*)(ProbeALCdevice*, ALenum);
static std::wstring ToWide(const std::string& text) {
    if (text.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (count <= 0) return L"(unprintable)";
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], count);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}
static bool ContainsNoCase(std::string text, const char* part) {
    for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::string p(part); for (char& c : p) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text.find(p) != std::string::npos;
}
static int ProbeCreativeOpenAL() {
    wchar_t sys[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) { std::wcerr << L"UNSUPPORTED: Windows system directory could not be resolved.\n"; return 10; }
    std::wstring path(sys); path += L"\\OpenAL32.dll";
    HMODULE dll = LoadLibraryW(path.c_str());
    if (!dll) { std::wcerr << L"UNSUPPORTED: system OpenAL32.dll is unavailable (Win32 error " << GetLastError() << L").\n"; return 11; }
    auto open = reinterpret_cast<ProbeOpenDevice>(GetProcAddress(dll, "alcOpenDevice"));
    auto create = reinterpret_cast<ProbeCreateContext>(GetProcAddress(dll, "alcCreateContext"));
    auto make = reinterpret_cast<ProbeMakeCurrent>(GetProcAddress(dll, "alcMakeContextCurrent"));
    auto destroy = reinterpret_cast<ProbeDestroyContext>(GetProcAddress(dll, "alcDestroyContext"));
    auto close = reinterpret_cast<ProbeCloseDevice>(GetProcAddress(dll, "alcCloseDevice"));
    auto get = reinterpret_cast<ProbeGetString>(GetProcAddress(dll, "alGetString"));
    auto cget = reinterpret_cast<ProbeCGetString>(GetProcAddress(dll, "alcGetString"));
    if (!open || !create || !make || !destroy || !close || !get || !cget) {
        std::wcerr << L"UNSUPPORTED: OpenAL32.dll is missing required OpenAL exports.\n"; FreeLibrary(dll); return 12;
    }
    ProbeALCdevice* dev = open(nullptr);
    if (!dev) { std::wcerr << L"UNSUPPORTED: OpenAL could not open the default device.\n"; FreeLibrary(dll); return 13; }
    ProbeALCcontext* ctx = create(dev, nullptr);
    if (!ctx) { std::wcerr << L"UNSUPPORTED: OpenAL context creation failed.\n"; close(dev); FreeLibrary(dll); return 14; }
    if (!make(ctx)) { std::wcerr << L"UNSUPPORTED: OpenAL context could not be made current.\n"; destroy(ctx); close(dev); FreeLibrary(dll); return 15; }
    const char* vendor = get(0xB001); const char* renderer = get(0xB003); const char* version = get(0xB002); const char* deviceName = cget(dev, 0x1005);
    std::string v = vendor ? vendor : "(unknown)", r = renderer ? renderer : "(unknown)", d = deviceName ? deviceName : "(unknown)", ver = version ? version : "(unknown)";
    std::wcout << L"OpenAL vendor: " << ToWide(v) << L"\nRenderer: " << ToWide(r) << L"\nDevice: " << ToWide(d) << L"\nVersion: " << ToWide(ver) << L"\n";
    bool creative = ContainsNoCase(v, "creative") || ContainsNoCase(r, "creative");
    make(nullptr); destroy(ctx); close(dev); FreeLibrary(dll);
    if (!creative) {
        std::wcerr << L"UNSUPPORTED: OpenAL is available, but its reported vendor/renderer/device does not identify Creative. Game Audio Bridge will not claim Creative hardware OpenAL is active.\n";
        return 16;
    }
    std::wcout << L"Creative OpenAL implementation/device detected. Note: generic OpenAL probing cannot independently prove that every sound path is hardware-accelerated.\n";
    return 0;
}

static std::wstring FullPath(const std::wstring& p) {
    std::wstring out(32768, L'\0');
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)out.size(), out.data(), nullptr);
    if (!n || n >= out.size()) return p;
    out.resize(n); return out;
}
static bool Inject(DWORD pid, const std::wstring& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n"; return false; }
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n"; CloseHandle(proc); return false; }
    if (!WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }
    auto load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, load, remote, 0, nullptr);
    if (!th) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }
    WaitForSingleObject(th, INFINITE);
    DWORD code = 0; GetExitCodeThread(th, &code);
    CloseHandle(th); VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc);
    if (!code) { std::wcerr << L"LoadLibraryW failed in target process. Check DLL bitness and dependencies.\n"; return false; }
    return true;
}
static bool LaunchSuspendedAndInject(const std::wstring& exe, const std::wstring& dll) {
    std::wstring fullExe = FullPath(exe);
    std::wstring fullDll = FullPath(dll);
    if (!std::filesystem::exists(fullExe)) { std::wcerr << L"Executable not found: " << fullExe << L"\n"; return false; }
    if (!std::filesystem::exists(fullDll)) { std::wcerr << L"DLL not found: " << fullDll << L"\n"; return false; }

    // Create the target suspended so its audio client cannot be created before the hook is loaded.
    std::wstring cmd = L"\"" + fullExe + L"\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    std::filesystem::path exePath(fullExe);
    std::wstring cwd = exePath.parent_path().wstring();
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(fullExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        std::wcerr << L"CreateProcess failed: " << GetLastError() << L"\n";
        return false;
    }

    bool ok = Inject(pi.dwProcessId, fullDll);
    if (!ok) {
        std::wcerr << L"Hook injection failed; the target will be closed rather than run without interception.\n";
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return false;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        std::wcerr << L"ResumeThread failed: " << GetLastError() << L"\n";
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return false;
    }
    std::wcout << L"Started with hook: " << fullExe << L" (PID " << pi.dwProcessId << L")\n";
    std::wcout << L"Diagnostics: %TEMP%\\GameAudioBridge.log\n";
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    std::wcout << L"Game Audio Bridge 1.0 — Creative OpenAL mode\n";
    if (argc == 2 && std::wstring(argv[1]) == L"--probe-openal") return ProbeCreativeOpenAL();
    if (argc == 4 && std::wstring(argv[1]) == L"--launch") {
        return LaunchSuspendedAndInject(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc == 4 && std::wstring(argv[1]) == L"--attach") {
        DWORD pid = wcstoul(argv[2], nullptr, 10);
        if (!pid) { std::wcerr << L"Invalid PID\n"; return 2; }
        auto dll = FullPath(argv[3]);
        if (!std::filesystem::exists(dll)) { std::wcerr << L"DLL not found: " << dll << L"\n"; return 2; }
        if (!Inject(pid, dll)) return 1;
        std::wcout << L"DLL loaded into PID " << pid << L". Existing audio clients may not be intercepted.\n";
        return 0;
    }
    std::wcout << L"Usage:\n"
               << L"  GameAudioBridgeLauncher.exe --probe-openal\n"
               << L"  GameAudioBridgeLauncher.exe --launch <full-path-to-exe> <full-path-to-hook.dll>\n"
               << L"  GameAudioBridgeLauncher.exe --attach <PID> <full-path-to-hook.dll>\n\n"
               << L"Recommended: run GameAudioBridge.exe to manage games and choose an EXE; it selects x86/x64 automatically.\n";
    return 2;
}
