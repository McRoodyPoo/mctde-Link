#include "pch.h"

#include <windows.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>

#ifdef MCTDE_LINK_SINGLE_DLL
extern "C" void McTDE_NetOverlay_OnProcessAttach(HMODULE hModule);
extern "C" void McTDE_NetOverlay_OnProcessDetach();
extern "C" __declspec(dllexport) void initialize_plugin();
extern "C" void McTDE_VersionChecker_Start();
#endif

HMODULE g_realD3D9 = NULL;

typedef void* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT SDKVersion, void** ppD3D);

Direct3DCreate9_t g_realDirect3DCreate9 = NULL;
Direct3DCreate9Ex_t g_realDirect3DCreate9Ex = NULL;

const char* HUB_INI_FILE = ".\\mctde-link.ini";
const char* HUB_LOG_FILE = "mctde-link.log";

HWND g_gameWindow = NULL;

static volatile LONG g_compatChainloadState = 0;
static bool g_hubLoggingConfigured = false;
static bool g_hubLoggingEnabled = false;

// ------------------------------------------------------------
// Path helpers
// ------------------------------------------------------------

std::string GetExeDirectory()
{
    char exePath[MAX_PATH] = { 0 };

    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0)
    {
        return ".";
    }

    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash)
    {
        return ".";
    }

    *lastSlash = '\0';
    return std::string(exePath);
}

bool IsAbsolutePathA(const char* path)
{
    if (!path || !path[0])
    {
        return false;
    }

    // C:\folder or D:/folder
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/'))
    {
        return true;
    }

    // UNC path: \\server\share
    return (path[0] == '\\' && path[1] == '\\');
}

std::string ResolveGamePath(const char* path)
{
    if (!path || !path[0])
    {
        return "";
    }

    if (IsAbsolutePathA(path))
    {
        return std::string(path);
    }

    std::string base = GetExeDirectory();
    if (!base.empty() && base[base.size() - 1] != '\\' && base[base.size() - 1] != '/')
    {
        base += "\\";
    }

    return base + path;
}

std::string GetHubIniPath()
{
    return ResolveGamePath(HUB_INI_FILE);
}

std::string GetHubLogPath()
{
    return ResolveGamePath(HUB_LOG_FILE);
}

std::string GetFileNameOnly(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos)
    {
        return path;
    }

    return path.substr(slash + 1);
}

bool EqualsIgnoreCaseA(const std::string& a, const std::string& b)
{
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

// ------------------------------------------------------------
// Logging
// ------------------------------------------------------------

bool IsHubLoggingEnabled()
{
    if (!g_hubLoggingConfigured)
    {
        std::string iniPath = GetHubIniPath();
        g_hubLoggingEnabled = GetPrivateProfileIntA("Settings", "EnableLogging", 0, iniPath.c_str()) != 0;
        g_hubLoggingConfigured = true;
    }

    return g_hubLoggingEnabled;
}

void CleanupHubLogIfDisabled()
{
    if (IsHubLoggingEnabled())
        return;

    std::string logPath = GetHubLogPath();
    DeleteFileA(logPath.c_str());
}

void WriteHubLog(const char* message)
{
    if (!IsHubLoggingEnabled())
        return;

    std::string logPath = GetHubLogPath();

    HANDLE file = CreateFileA(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
        WriteFile(file, "\r\n", 2, &written, NULL);
        CloseHandle(file);
    }
}

void WriteHubLogString(const std::string& message)
{
    WriteHubLog(message.c_str());
}

std::string GetWindowsErrorMessage(DWORD errorCode)
{
    char errorText[1024] = { 0 };

    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        0,
        errorText,
        sizeof(errorText),
        NULL
    );

    return std::string(errorText);
}

// ------------------------------------------------------------
// Sean-style helper exports
// Phantom_Break.dll expects these from d3d9.dll.
// ------------------------------------------------------------

HWND __cdecl get_game_window()
{
    if (g_gameWindow)
    {
        return g_gameWindow;
    }

    g_gameWindow = FindWindowA(NULL, "DARK SOULS");

    if (!g_gameWindow)
    {
        g_gameWindow = GetForegroundWindow();
    }

    return g_gameWindow;
}

bool __cdecl console_is_open()
{
    return false;
}

bool __cdecl print_console(std::string& message)
{
    std::string logMessage = "[Phantom_Break] ";
    logMessage += message;

    WriteHubLogString(logMessage);

    return true;
}

// ------------------------------------------------------------
// Load real system d3d9.dll
// ------------------------------------------------------------

void LoadRealD3D9()
{
    if (g_realD3D9)
    {
        return;
    }

    char systemPath[MAX_PATH] = { 0 };

    if (GetSystemDirectoryA(systemPath, MAX_PATH) == 0)
    {
        WriteHubLog("GetSystemDirectoryA failed.");
        return;
    }

    std::string realPath = std::string(systemPath) + "\\d3d9.dll";

    g_realD3D9 = LoadLibraryA(realPath.c_str());

    if (!g_realD3D9)
    {
        DWORD errorCode = GetLastError();

        std::string message = "Failed to load real system d3d9.dll. Error code: ";
        message += std::to_string(errorCode);
        message += ". Message: ";
        message += GetWindowsErrorMessage(errorCode);

        WriteHubLogString(message);
        return;
    }

    WriteHubLog("Loaded real system d3d9.dll.");

    g_realDirect3DCreate9 =
        (Direct3DCreate9_t)GetProcAddress(g_realD3D9, "Direct3DCreate9");

    g_realDirect3DCreate9Ex =
        (Direct3DCreate9Ex_t)GetProcAddress(g_realD3D9, "Direct3DCreate9Ex");

    if (g_realDirect3DCreate9)
        WriteHubLog("Found Direct3DCreate9.");
    else
        WriteHubLog("Could not find Direct3DCreate9.");

    if (g_realDirect3DCreate9Ex)
        WriteHubLog("Found Direct3DCreate9Ex.");
    else
        WriteHubLog("Could not find Direct3DCreate9Ex.");
}

// ------------------------------------------------------------
// Plugin support
// ------------------------------------------------------------

typedef void(__cdecl* InitializePluginFunc)();

void TryInitializePlugin(HMODULE loadedDll, const char* dllName)
{
    FARPROC initProc = GetProcAddress(loadedDll, "initialize_plugin");

    if (!initProc)
    {
        std::string message = dllName;
        message += " has no initialize_plugin export; leaving it loaded as a DllMain-only compatibility DLL.";
        WriteHubLogString(message);
        return;
    }

    std::string message = "Calling initialize_plugin for ";
    message += dllName;
    message += ".";

    WriteHubLogString(message);

    InitializePluginFunc initializePlugin = (InitializePluginFunc)initProc;
    initializePlugin();

    message = "Finished initialize_plugin for ";
    message += dllName;
    message += ".";

    WriteHubLogString(message);

    FARPROC renderProc = GetProcAddress(loadedDll, "render_overlay");

    if (renderProc)
    {
        message = "Found render_overlay in ";
        message += dllName;
        message += ", but mctde-Link does not call foreign render_overlay callbacks. DllMain/initialize_plugin startup is still supported.";

        WriteHubLogString(message);
    }
}

// ------------------------------------------------------------
// Load one DLL
// ------------------------------------------------------------

void LoadOneDll(const char* dllName)
{
    if (!dllName || strlen(dllName) == 0)
    {
        return;
    }

    std::string resolvedPath = ResolveGamePath(dllName);
    std::string fileName = GetFileNameOnly(resolvedPath);

    // Do not recursively load another d3d9 proxy through the compatibility chainloader.
    if (EqualsIgnoreCaseA(fileName, "d3d9.dll"))
    {
        std::string skipMessage = "Skipping ";
        skipMessage += resolvedPath;
        skipMessage += " because d3d9.dll is mctde-Link's own proxy slot.";
        WriteHubLogString(skipMessage);
        return;
    }

    std::string attemptMessage = "Attempting to load compatibility DLL ";
    attemptMessage += resolvedPath;
    attemptMessage += ".";

    WriteHubLogString(attemptMessage);

    HMODULE loadedDll = LoadLibraryA(resolvedPath.c_str());

    if (!loadedDll)
    {
        DWORD errorCode = GetLastError();

        std::string message = "Failed to load ";
        message += resolvedPath;
        message += ". Error code: ";
        message += std::to_string(errorCode);
        message += ". Message: ";
        message += GetWindowsErrorMessage(errorCode);

        WriteHubLogString(message);
        return;
    }

    std::string successMessage = "Loaded compatibility DLL ";
    successMessage += resolvedPath;
    successMessage += ".";

    WriteHubLogString(successMessage);

    TryInitializePlugin(loadedDll, resolvedPath.c_str());
}

// ------------------------------------------------------------
// Load DLLs from mctde-link.ini
// ------------------------------------------------------------

void LoadGenericDlls()
{
    WriteHubLog("Loading GenericDLL entries from mctde-link.ini.");

    std::string iniPath = GetHubIniPath();
    char dllName[MAX_PATH] = { 0 };

    for (int i = 0; i < 32; i++)
    {
        ZeroMemory(dllName, sizeof(dllName));

        char keyName[32] = { 0 };
        sprintf_s(keyName, sizeof(keyName), "GenericDLL%d", i);

        GetPrivateProfileStringA(
            "DLLs",
            keyName,
            "",
            dllName,
            MAX_PATH,
            iniPath.c_str()
        );

        if (strlen(dllName) == 0)
        {
            continue;
        }

        LoadOneDll(dllName);
    }

    WriteHubLog("Finished loading GenericDLL entries.");
}

void LoadDllsFromFolder(const char* folderName)
{
    if (!folderName || !folderName[0])
    {
        return;
    }

    std::string folder = ResolveGamePath(folderName);
    std::string mask = folder;
    if (!mask.empty() && mask[mask.size() - 1] != '\\' && mask[mask.size() - 1] != '/')
    {
        mask += "\\";
    }
    mask += "*.dll";

    WIN32_FIND_DATAA findData;
    ZeroMemory(&findData, sizeof(findData));

    HANDLE find = FindFirstFileA(mask.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        std::string message = "Compatibility chainload folder not found or empty: ";
        message += folder;
        WriteHubLogString(message);
        return;
    }

    std::vector<std::string> dlls;

    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }

        std::string name = findData.cFileName;
        if (EqualsIgnoreCaseA(name, "d3d9.dll"))
        {
            continue;
        }

        std::string full = folder;
        if (!full.empty() && full[full.size() - 1] != '\\' && full[full.size() - 1] != '/')
        {
            full += "\\";
        }
        full += name;
        dlls.push_back(full);
    }
    while (FindNextFileA(find, &findData));

    FindClose(find);

    std::sort(dlls.begin(), dlls.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });

    std::string message = "Loading compatibility chainload folder: ";
    message += folder;
    WriteHubLogString(message);

    for (size_t i = 0; i < dlls.size(); i++)
    {
        LoadOneDll(dlls[i].c_str());
    }

    WriteHubLog("Finished loading compatibility chainload folder.");
}

void LoadCompatibilityDllsOnce()
{
    LONG previous = InterlockedCompareExchange(&g_compatChainloadState, 1, 0);

    if (previous == 2)
    {
        return;
    }

    if (previous == 1)
    {
        // Re-entrant LoadLibrary/DllMain paths can happen with hook DLLs. Do not wait here;
        // waiting can deadlock if the DLL being loaded touches d3d9 during its own startup.
        WriteHubLog("Compatibility DLL chainload pass already in progress; skipping re-entrant call.");
        return;
    }

    WriteHubLog("Starting compatibility DLL chainload pass.");

    LoadGenericDlls();

    std::string iniPath = GetHubIniPath();
    char folderName[MAX_PATH] = { 0 };
    GetPrivateProfileStringA(
        "Compatibility",
        "ChainloadFolder",
        "mctde-Link_Chainload",
        folderName,
        MAX_PATH,
        iniPath.c_str()
    );

    if (strlen(folderName) > 0)
    {
        LoadDllsFromFolder(folderName);
    }

    WriteHubLog("Finished compatibility DLL chainload pass.");
    InterlockedExchange(&g_compatChainloadState, 2);
}

// ------------------------------------------------------------
// Built-in mctde-Link modules
// ------------------------------------------------------------

void StartBuiltInModules()
{
    // Load third-party compatibility DLLs before the built-in overlay begins its delayed hook setup.
    // This lets DllMain-driven mods such as PTDE Practice Tool's dinput8/hudhook DLL install early.
    LoadCompatibilityDllsOnce();

#ifdef MCTDE_LINK_SINGLE_DLL
    WriteHubLog("Starting built-in MCTDE_NetOverlay module.");
    initialize_plugin();

    WriteHubLog("Starting built-in VersionChecker module.");
    McTDE_VersionChecker_Start();
#endif
}

// ------------------------------------------------------------
// Main hub thread
// ------------------------------------------------------------

DWORD WINAPI HubThread(LPVOID)
{
    CleanupHubLogIfDisabled();
    WriteHubLog("----------------------------------------");
    WriteHubLog("MCTDE d3d9 hub started.");

    g_gameWindow = FindWindowA(NULL, "DARK SOULS");

    if (g_gameWindow)
        WriteHubLog("Found DARK SOULS window.");
    else
        WriteHubLog("Could not find DARK SOULS window yet.");

    LoadRealD3D9();
    StartBuiltInModules();

    WriteHubLog("mctde-Link single-dll d3d9 finished loading.");

    return 0;
}

// ------------------------------------------------------------
// Direct3D exports
// The d3d9.def file controls export names.
// ------------------------------------------------------------

extern "C" void* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    LoadCompatibilityDllsOnce();
    LoadRealD3D9();

    if (!g_realDirect3DCreate9)
    {
        WriteHubLog("Direct3DCreate9 failed because real function was not found.");
        return NULL;
    }

    void* result = g_realDirect3DCreate9(SDKVersion);

    if (result)
        WriteHubLog("Direct3DCreate9 succeeded.");
    else
        WriteHubLog("Direct3DCreate9 returned NULL.");

    return result;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, void** ppD3D)
{
    LoadCompatibilityDllsOnce();
    LoadRealD3D9();

    if (!g_realDirect3DCreate9Ex)
    {
        WriteHubLog("Direct3DCreate9Ex failed because real function was not found.");
        return E_FAIL;
    }

    HRESULT result = g_realDirect3DCreate9Ex(SDKVersion, ppD3D);

    if (SUCCEEDED(result))
        WriteHubLog("Direct3DCreate9Ex succeeded.");
    else
        WriteHubLog("Direct3DCreate9Ex failed.");

    return result;
}

// ------------------------------------------------------------
// DLL entry point
// ------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

#ifdef MCTDE_LINK_SINGLE_DLL
        McTDE_NetOverlay_OnProcessAttach(hModule);
#endif

        HANDLE thread = CreateThread(
            NULL,
            0,
            HubThread,
            NULL,
            0,
            NULL
        );

        if (thread)
        {
            CloseHandle(thread);
        }
    }

    if (reason == DLL_PROCESS_DETACH)
    {
#ifdef MCTDE_LINK_SINGLE_DLL
        McTDE_NetOverlay_OnProcessDetach();
#endif

        if (g_realD3D9)
        {
            FreeLibrary(g_realD3D9);
            g_realD3D9 = NULL;
        }
    }

    return TRUE;
}
