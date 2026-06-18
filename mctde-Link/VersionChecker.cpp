#include "pch.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <string>
#include <fstream>
#include <cstring>
#include <sstream>
#include <vector>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

// Installed versions bundled with this release.
// Change these every time you release a new version.
#define CURRENT_MCTDE_VERSION "0.88"
#define CURRENT_MCTDE_LINK_VERSION "0.1.3"

// Your GitHub raw file info.
// Full URL:
// https://raw.githubusercontent.com/McRoodyPoo/mctde-Link/main/latest.txt
#define VERSION_HOST L"raw.githubusercontent.com"
#define VERSION_PATH L"/McRoodyPoo/mctde-Link/main/latest.txt"

#define MCTDE_DOWNLOAD_URL "https://www.nexusmods.com/darksouls/mods/1926?tab=files"
// Releases list page (not a version-pinned zip), so out-of-date users always land on the
// page where they can grab the newest release, regardless of which version they run.
#define MCTDE_LINK_DOWNLOAD_URL "https://github.com/McRoodyPoo/mctde-Link/releases"

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

static std::string NormalizeVersionKey(const std::string& text)
{
    std::string result = Trim(text);

    for (size_t i = 0; i < result.size(); i++)
    {
        char c = result[i];

        if (c >= 'A' && c <= 'Z')
        {
            result[i] = static_cast<char>(c - 'A' + 'a');
        }
        else if (c == '_')
        {
            result[i] = '-';
        }
    }

    return result;
}

static bool IsDigitsOnly(const std::string& text)
{
    if (text.empty())
    {
        return false;
    }

    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return false;
        }
    }

    return true;
}

static bool ParseVersionParts(const std::string& version, std::vector<int>& parts)
{
    parts.clear();

    std::stringstream stream(Trim(version));
    std::string part;

    while (std::getline(stream, part, '.'))
    {
        part = Trim(part);

        if (!IsDigitsOnly(part))
        {
            return false;
        }

        parts.push_back(atoi(part.c_str()));
    }

    return !parts.empty();
}

static int CompareVersions(const std::string& left, const std::string& right)
{
    std::vector<int> leftParts;
    std::vector<int> rightParts;

    if (!ParseVersionParts(left, leftParts) || !ParseVersionParts(right, rightParts))
    {
        return Trim(left) == Trim(right) ? 0 : 1;
    }

    size_t count = leftParts.size() > rightParts.size() ? leftParts.size() : rightParts.size();

    for (size_t i = 0; i < count; i++)
    {
        int leftValue = i < leftParts.size() ? leftParts[i] : 0;
        int rightValue = i < rightParts.size() ? rightParts[i] : 0;

        if (leftValue > rightValue)
        {
            return 1;
        }

        if (leftValue < rightValue)
        {
            return -1;
        }
    }

    return 0;
}

static bool IsOutOfDate(const std::string& latestVersion, const std::string& installedVersion)
{
    if (Trim(latestVersion).empty())
    {
        return false;
    }

    return CompareVersions(latestVersion, installedVersion) > 0;
}

struct LatestVersions
{
    std::string mctde;
    std::string mctdeLink;
};

static LatestVersions ParseLatestVersions(const std::string& text)
{
    LatestVersions versions;
    std::stringstream stream(text);
    std::string line;

    while (std::getline(stream, line))
    {
        size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }

        line = Trim(line);
        if (line.empty())
        {
            continue;
        }

        size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            separator = line.find(':');
        }

        if (separator == std::string::npos)
        {
            if (versions.mctdeLink.empty())
            {
                versions.mctdeLink = line;
            }

            continue;
        }

        std::string key = NormalizeVersionKey(line.substr(0, separator));
        std::string value = Trim(line.substr(separator + 1));

        if (key == "mctde")
        {
            versions.mctde = value;
        }
        else if (key == "mctde-link" || key == "mctdelink")
        {
            versions.mctdeLink = value;
        }
    }

    return versions;
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
// Downloads the latest version manifest from GitHub.
// ------------------------------------------------------------
std::string DownloadLatestManifest()
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

    std::string latestManifest = DownloadLatestManifest();

    if (latestManifest.empty())
    {
        WriteLog("Could not check latest version manifest.");
        return 0;
    }

    LatestVersions latestVersions = ParseLatestVersions(latestManifest);

    WriteLog("Installed mctde version: " + std::string(CURRENT_MCTDE_VERSION));
    WriteLog("Latest mctde version: " + latestVersions.mctde);
    WriteLog("Installed mctde-link version: " + std::string(CURRENT_MCTDE_LINK_VERSION));
    WriteLog("Latest mctde-link version: " + latestVersions.mctdeLink);

    bool mctdeOutOfDate = IsOutOfDate(latestVersions.mctde, CURRENT_MCTDE_VERSION);
    bool linkOutOfDate = IsOutOfDate(latestVersions.mctdeLink, CURRENT_MCTDE_LINK_VERSION);

    if (!mctdeOutOfDate && !linkOutOfDate)
    {
        WriteLog("mctde and mctde-Link are up to date.");
        return 0;
    }

    const char* downloadUrl = MCTDE_LINK_DOWNLOAD_URL;
    const char* title = "mctde-link Update Required";
    std::string popupMessage;

    if (mctdeOutOfDate)
    {
        WriteLog("mctde update available.");

        downloadUrl = MCTDE_DOWNLOAD_URL;
        title = "mctde Update Required";
        popupMessage =
            "mctde is out of date. Open the download page?\n\n"
            "Installed mctde version: " + std::string(CURRENT_MCTDE_VERSION) + "\n"
            "Latest mctde version: " + latestVersions.mctde + "\n\n"
            "Dark Souls will close either way so you can install the update.";
    }
    else
    {
        WriteLog("mctde-link update available.");

        popupMessage =
            "mctde-link is out of date. Open the download page?\n\n"
            "Installed mctde-link version: " + std::string(CURRENT_MCTDE_LINK_VERSION) + "\n"
            "Latest mctde-link version: " + latestVersions.mctdeLink + "\n\n"
            "Dark Souls will close either way so you can install the update.";
    }

    int result = MessageBoxA(
        NULL,
        popupMessage.c_str(),
        title,
        MB_YESNO | MB_ICONWARNING | MB_TOPMOST
    );

    if (result == IDYES)
    {
        WriteLog("User chose to update. Opening download page, then closing Dark Souls.");
        ShellExecuteA(
            NULL,
            "open",
            downloadUrl,
            NULL,
            NULL,
            SW_SHOWNORMAL
        );
        Sleep(750); // give the browser a moment to launch before we exit
    }
    else
    {
        WriteLog("User declined update. Closing Dark Souls.");
    }

    // Either way, close Dark Souls so the player can install the update
    // (d3d9.dll can't be replaced while the game holds it open).
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);

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
