#include "pch.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <string>
#include <fstream>
#include <cstring>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

// Your installed mod version.
// Change this every time you release a new version.
#define CURRENT_VERSION "0.88"

// Your GitHub raw file info.
// Full URL:
// https://raw.githubusercontent.com/McRoodyPoo/mctde/refs/heads/main/latest.txt
#define VERSION_HOST L"raw.githubusercontent.com"
#define VERSION_PATH L"/McRoodyPoo/mctde/refs/heads/main/latest.txt"

// Change this to your actual mod download page later.
#define DOWNLOAD_URL "https://github.com/McRoodyPoo/mctde"

static bool g_versionLoggingConfigured = false;
static bool g_versionLoggingEnabled = false;

// ------------------------------------------------------------
// Removes spaces/newlines from the beginning and end of text.
// ------------------------------------------------------------
std::string Trim(const std::string& text)
{
    size_t start = text.find_first_not_of(" \r\n\t");
    size_t end = text.find_last_not_of(" \r\n\t");

    if (start == std::string::npos)
    {
        return "";
    }

    return text.substr(start, end - start + 1);
}

static std::string GetDllDirectory()
{
    char path[MAX_PATH] = { 0 };
    HMODULE module = GetModuleHandleA("d3d9.dll");
    if (!module || GetModuleFileNameA(module, path, MAX_PATH) == 0)
        return ".";

    char* lastSlash = strrchr(path, '\\');
    if (!lastSlash)
        return ".";

    *lastSlash = '\0';
    return std::string(path);
}

static std::string GetVersionIniPath()
{
    std::string dir = GetDllDirectory();
    if (!dir.empty() && dir[dir.size() - 1] != '\\' && dir[dir.size() - 1] != '/')
        dir += "\\";
    return dir + "mctde-link.ini";
}

static std::string GetVersionLogPath()
{
    std::string dir = GetDllDirectory();
    if (!dir.empty() && dir[dir.size() - 1] != '\\' && dir[dir.size() - 1] != '/')
        dir += "\\";
    return dir + "VersionCheck.log";
}

static bool IsVersionLoggingEnabled()
{
    if (!g_versionLoggingConfigured)
    {
        std::string iniPath = GetVersionIniPath();
        g_versionLoggingEnabled = GetPrivateProfileIntA("Settings", "EnableLogging", 0, iniPath.c_str()) != 0;
        g_versionLoggingConfigured = true;
    }

    return g_versionLoggingEnabled;
}

static void CleanupVersionLogIfDisabled()
{
    if (IsVersionLoggingEnabled())
        return;

    std::string logPath = GetVersionLogPath();
    DeleteFileA(logPath.c_str());
}

// ------------------------------------------------------------
// Writes messages to VersionCheck.log.
// ------------------------------------------------------------
void WriteLog(const std::string& message)
{
    if (!IsVersionLoggingEnabled())
        return;

    std::ofstream log(GetVersionLogPath().c_str(), std::ios::app);

    if (log.is_open())
    {
        log << message << std::endl;
    }
}

// ------------------------------------------------------------
// Downloads the latest version number from GitHub.
// ------------------------------------------------------------
std::string DownloadLatestVersion()
{
    std::string result;

    HINTERNET hSession = WinHttpOpen(
        L"MCTDEVersionChecker/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession)
    {
        WriteLog("WinHttpOpen failed.");
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        VERSION_HOST,
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (!hConnect)
    {
        WriteLog("WinHttpConnect failed.");
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        VERSION_PATH,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (!hRequest)
    {
        WriteLog("WinHttpOpenRequest failed.");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    BOOL sent = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    );

    if (!sent)
    {
        WriteLog("WinHttpSendRequest failed.");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    BOOL received = WinHttpReceiveResponse(hRequest, NULL);

    if (!received)
    {
        WriteLog("WinHttpReceiveResponse failed.");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);

    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX
    );

    if (statusCode != 200)
    {
        WriteLog("HTTP request failed. Status code: " + std::to_string(statusCode));

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD size = 0;

    do
    {
        DWORD downloaded = 0;

        if (!WinHttpQueryDataAvailable(hRequest, &size))
        {
            WriteLog("WinHttpQueryDataAvailable failed.");
            break;
        }

        if (size == 0)
        {
            break;
        }

        char* buffer = new char[size + 1];
        ZeroMemory(buffer, size + 1);

        if (WinHttpReadData(hRequest, buffer, size, &downloaded))
        {
            result.append(buffer, downloaded);
        }
        else
        {
            WriteLog("WinHttpReadData failed.");
        }

        delete[] buffer;

    } while (size > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return Trim(result);
}

// ------------------------------------------------------------
// This runs after the DLL loads.
// Do NOT do internet stuff directly inside DllMain.
// ------------------------------------------------------------
DWORD WINAPI VersionCheckThread(LPVOID)
{
    WriteLog("----------------------------------------");
    WriteLog("Version checker started.");

    std::string currentVersion = CURRENT_VERSION;
    std::string latestVersion = DownloadLatestVersion();

    if (latestVersion.empty())
    {
        WriteLog("Could not check latest version.");
        return 0;
    }

    WriteLog("Installed version: " + currentVersion);
    WriteLog("Latest version: " + latestVersion);

    if (latestVersion != currentVersion)
    {
        WriteLog("Update available.");

        std::string popupMessage =
            "A new version of MCTDE is available.\n\n"
            "Installed version: " + currentVersion + "\n"
            "Latest version: " + latestVersion + "\n\n"
            "You can keep playing, but you may run into bugs that have already been fixed.\n\n"
            "Open the download page now?";

        int result = MessageBoxA(
            NULL,
            popupMessage.c_str(),
            "MCTDE Update Available",
            MB_YESNO | MB_ICONWARNING | MB_TOPMOST
        );

        if (result == IDYES)
        {
            ShellExecuteA(
                NULL,
                "open",
                DOWNLOAD_URL,
                NULL,
                NULL,
                SW_SHOWNORMAL
            );
        }
    }
    else
    {
        WriteLog("Mod is up to date.");
    }

    return 0;
}

static void StartVersionCheckThread()
{
    CleanupVersionLogIfDisabled();

    HANDLE thread = CreateThread(
        NULL,
        0,
        VersionCheckThread,
        NULL,
        0,
        NULL
    );

    if (thread)
    {
        CloseHandle(thread);
    }
}

#ifdef MCTDE_LINK_SINGLE_DLL
extern "C" void McTDE_VersionChecker_Start()
{
    StartVersionCheckThread();
}
#else
// ------------------------------------------------------------
// DLL entry point.
// Keep this tiny.
// ------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        StartVersionCheckThread();
    }

    return TRUE;
}
#endif
