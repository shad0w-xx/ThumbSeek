// thumb25-setup: one-click install/uninstall of thumb25.dll and its FFmpeg DLLs (which must sit next to this exe).
// The dialog runs as the normal user; only the actual install/uninstall step asks for admin (UAC),
// by relaunching this exe with /install or /uninstall.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cwchar>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t kInprocKey[] = L"Software\\Classes\\CLSID\\{FE46C886-E672-45E5-8B96-DD23C299B64F}\\InprocServer32";
enum { ID_INSTALL = 100, ID_UNINSTALL };

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    wchar_t* p = nullptr;
    std::wstring s = SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) ? p : L"";
    CoTaskMemFree(p);
    return s;
}

static std::wstring InstallDir() { return KnownFolder(FOLDERID_ProgramFiles) + L"\\thumb25"; }

static std::wstring InstalledDll() {
    wchar_t v[MAX_PATH * 2];
    DWORD cb = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kInprocKey, nullptr, RRF_RT_REG_SZ, nullptr, v, &cb) == ERROR_SUCCESS ? v : L"";
}

static HRESULT CallDll(const std::wstring& dll, const char* fn) {
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);  // finds FFmpeg DLLs beside it
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    auto f = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(m, fn);
    HRESULT hr = f ? f() : E_FAIL;
    FreeLibrary(m);
    return hr;
}

// Deletes a file. If Explorer has it loaded, moves it aside and deletes it at the next reboot.
static bool RemoveFile(const std::wstring& path) {
    if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    std::wstring aside = path + L"." + std::to_wstring(GetTickCount64()) + L".old";
    if (!MoveFileExW(path.c_str(), aside.c_str(), 0)) return false;
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

// thumb25.dll and the FFmpeg DLLs it needs (version numbers in the names change with FFmpeg updates).
static const wchar_t* kFiles[] = {L"thumb25.dll",   L"avformat-*.dll", L"avcodec-*.dll",
                                  L"avutil-*.dll",  L"swscale-*.dll",  L"swresample-*.dll"};

// Calls fn(file name) for each of our files in dir; stops and returns false if fn does.
template <class F> static bool ForEachFile(const std::wstring& dir, F fn) {
    for (const wchar_t* pattern : kFiles) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool ok = true;
        do ok = fn(std::wstring(fd.cFileName));
        while (ok && FindNextFileW(h, &fd));
        FindClose(h);
        if (!ok) return false;
    }
    return true;
}

// Explorer draws film-strip "movie rails" on video thumbnails when Treatment is 3 (Windows' default; 0 = none),
// and stamps the default app's icon on them unless TypeOverlay is an empty string (normally the value is absent).
static const wchar_t kVideoKey[] = L"Software\\Classes\\SystemFileAssociations\\video";

static bool RailsOff() {
    DWORD v = 3, cb = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, kVideoKey, L"Treatment", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}

static void SetRails(bool on) {
    DWORD v = on ? 3 : 0;
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kVideoKey, L"Treatment", REG_DWORD, &v, sizeof(v));
    if (on)
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kVideoKey, L"TypeOverlay");
    else
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kVideoKey, L"TypeOverlay", REG_SZ, L"", sizeof(wchar_t));
}

// --- Elevated part ---

static HRESULT AdminInstall(bool rails) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring src = exe;
    src = src.substr(0, src.find_last_of(L'\\'));
    std::wstring dir = InstallDir();
    if (GetFileAttributesW((src + L"\\thumb25.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (_wcsicmp(src.c_str(), dir.c_str()) != 0) {  // running from the install folder itself: nothing to copy
        CreateDirectoryW(dir.c_str(), nullptr);
        ForEachFile(dir, [&](const std::wstring& f) { return RemoveFile(dir + L"\\" + f), true; });  // old versions
        if (!ForEachFile(src, [&](const std::wstring& f) {
                return CopyFileW((src + L"\\" + f).c_str(), (dir + L"\\" + f).c_str(), FALSE) != 0;
            }))
            return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = CallDll(dir + L"\\thumb25.dll", "DllRegisterServer");
    if (SUCCEEDED(hr)) SetRails(rails);
    return hr;
}

static HRESULT AdminUninstall() {
    std::wstring dll = InstalledDll();
    if (dll.empty()) return S_OK;
    HRESULT hr = CallDll(dll, "DllUnregisterServer");  // restores Windows' own thumbnail handler
    if (FAILED(hr)) return hr;
    SetRails(true);  // Windows' default
    std::wstring dir = InstallDir();
    if (_wcsnicmp(dll.c_str(), (dir + L"\\").c_str(), dir.size() + 1) == 0) {  // only delete our own folder
        ForEachFile(dir, [&](const std::wstring& f) { return RemoveFile(dir + L"\\" + f), true; });
        if (!RemoveDirectoryW(dir.c_str())) MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return S_OK;
}

// --- Normal-user part ---

static HRESULT RunElevated(const wchar_t* arg) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = arg;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return HRESULT_FROM_WIN32(GetLastError());  // includes "No" on the UAC prompt
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = (DWORD)E_FAIL;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (HRESULT)code;
}

// Closes (kill=true) or counts this session's explorer.exe processes.
static int Explorers(bool kill) {
    DWORD mySession = 0, s;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    int n = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") || !ProcessIdToSessionId(pe.th32ProcessID, &s) || s != mySession)
            continue;
        ++n;
        if (!kill) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return n;
}

// Restarts Explorer so it picks up the change, and wipes the thumbnail cache so every video is redone.
static void RefreshExplorer() {
    Explorers(true);
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Windows\\Explorer\\";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"thumbcache_*.db").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do DeleteFileW((dir + fd.cFileName).c_str());
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    Sleep(1500);  // Windows may restart the shell by itself
    if (!Explorers(false)) ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

static HRESULT CALLBACK OnDialog(HWND h, UINT msg, WPARAM, LPARAM, LONG_PTR installed) {
    if (msg == TDN_CREATED) {
        SendMessageW(h, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, ID_INSTALL, TRUE);
        SendMessageW(h, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, ID_UNINSTALL, TRUE);
        SendMessageW(h, TDM_ENABLE_BUTTON, ID_UNINSTALL, installed);
    }
    return S_OK;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!wcscmp(cmd, L"/install")) return AdminInstall(true);
    if (!wcscmp(cmd, L"/install-norails")) return AdminInstall(false);
    if (!wcscmp(cmd, L"/uninstall")) return AdminUninstall();

    for (;;) {
        std::wstring dll = InstalledDll();
        std::wstring status = dll.empty() ? L"Status: not installed." : L"Status: installed\n" + dll;
        TASKDIALOG_BUTTON buttons[] = {
            {ID_INSTALL, L"Install\nCopies thumb25 to Program Files and makes Explorer use it. Run again to change the option below."},
            {ID_UNINSTALL, L"Uninstall\nRemoves it and brings back Windows' own video thumbnails."}};
        TASKDIALOGCONFIG tc = {sizeof(tc)};
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        tc.pszWindowTitle = L"thumb25 setup";
        tc.pszMainInstruction = L"Video thumbnails from 25% into the video";
        tc.pszContent = status.c_str();
        tc.pButtons = buttons;
        tc.cButtons = ARRAYSIZE(buttons);
        tc.pfCallback = OnDialog;
        tc.lpCallbackData = !dll.empty();
        tc.pszVerificationText = L"Clean thumbnails: no film-strip borders (movie rails) and no app icon";
        if (RailsOff()) tc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
        int pressed = 0;
        BOOL noRails = FALSE;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, &noRails))) return 1;
        if (pressed != ID_INSTALL && pressed != ID_UNINSTALL) return 0;

        HRESULT hr = RunElevated(pressed == ID_UNINSTALL ? L"/uninstall" : noRails ? L"/install-norails" : L"/install");
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) continue;  // said No to UAC
        if (SUCCEEDED(hr)) RefreshExplorer();
        wchar_t msg[200];
        if (SUCCEEDED(hr)) swprintf_s(msg, L"Explorer was restarted and video thumbnails will be regenerated as you open folders.");
        else swprintf_s(msg, L"Error 0x%08X.", (unsigned)hr);
        TaskDialog(nullptr, nullptr, L"thumb25 setup", SUCCEEDED(hr) ? L"Done" : L"That didn't work", msg,
                   TDCBF_OK_BUTTON, SUCCEEDED(hr) ? TD_INFORMATION_ICON : TD_ERROR_ICON, nullptr);
    }
}
