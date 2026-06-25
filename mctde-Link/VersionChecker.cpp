#include "pch.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>

#include <string>
#include <fstream>
#include <cstring>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <cctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

// Installed versions bundled with this release.
// Change these every time you release a new version.
#define CURRENT_MCTDE_VERSION "0.88"
#define CURRENT_MCTDE_LINK_VERSION "0.4.5"

// Your GitHub raw file host.
#define VERSION_HOST L"raw.githubusercontent.com"

// ============================================================
// Release channel (sandboxing)
// ------------------------------------------------------------
// PRODUCTION builds read the live latest.txt and pull updates from the real
// "latest release", so every shipped DLL watches the same files real users get.
//
// TEST builds read a SEPARATE TestingVersion.txt and pull from a fixed GitHub
// PRE-RELEASE tagged "test". That lets you bump the test manifest and publish
// test zips without prompting or updating anyone on a production build:
//   * Production clients only ever read latest.txt (you leave it alone), and
//   * a pre-release never becomes /releases/latest, so production clients can't
//     even download the test zip.
//
// MCTDE_LINK_TEST_CHANNEL is defined for the Debug configuration in the .vcxproj,
// so the rule is simply: build Debug = sandbox, build Release = ship to users.
//
// To test end-to-end you need (test channel only):
//   1. A TestingVersion.txt on main (same format as latest.txt), and
//   2. A GitHub *pre-release* tagged "test" with a mctde-Link.zip asset.
// ============================================================
#ifdef MCTDE_LINK_TEST_CHANNEL
    #define MCTDE_LINK_CHANNEL_LABEL "TEST"
    // https://raw.githubusercontent.com/McRoodyPoo/mctde-Link/main/TestingVersion.txt
    #define VERSION_PATH L"/McRoodyPoo/mctde-Link/main/TestingVersion.txt"
    #define MCTDE_LINK_RELEASE_ZIP_URL L"https://github.com/McRoodyPoo/mctde-Link/releases/download/test/mctde-Link.zip"
#else
    #define MCTDE_LINK_CHANNEL_LABEL "RELEASE"
    // https://raw.githubusercontent.com/McRoodyPoo/mctde-Link/main/latest.txt
    #define VERSION_PATH L"/McRoodyPoo/mctde-Link/main/latest.txt"
    #define MCTDE_LINK_RELEASE_ZIP_URL L"https://github.com/McRoodyPoo/mctde-Link/releases/latest/download/mctde-Link.zip"
#endif

#define MCTDE_DOWNLOAD_URL "https://www.nexusmods.com/darksouls/mods/1926?tab=files"
// Releases list page (not a version-pinned zip), so out-of-date users always land on the
// page where they can grab the newest release, regardless of which version they run.
#define MCTDE_LINK_DOWNLOAD_URL "https://github.com/McRoodyPoo/mctde-Link/releases"

// Where the "No" button (and any auto-update fallback) sends the user.
#define MCTDE_LINK_RELEASES_LATEST_URL "https://github.com/McRoodyPoo/mctde-Link/releases/latest"

// The standalone launcher (separate repo). Fetched, with consent, when it's missing.
// RELEASE REQUIREMENT: attach "mctde_launcher.exe" to the launcher's GitHub release.
// OPTIONAL: also attach "mctde_launcher.exe.sha256" (first token = lowercase hex SHA-256)
// to enable a strict integrity check on top of HTTPS.
#define MCTDE_LAUNCHER_EXE_URL     L"https://github.com/McRoodyPoo/mctde-Launcher/releases/latest/download/mctde_launcher.exe"
#define MCTDE_LAUNCHER_SHA256_URL  L"https://github.com/McRoodyPoo/mctde-Launcher/releases/latest/download/mctde_launcher.exe.sha256"
#define MCTDE_LAUNCHER_RELEASES_URL "https://github.com/McRoodyPoo/mctde-Launcher/releases/latest"

// The update zip URL (MCTDE_LINK_RELEASE_ZIP_URL) is defined per-channel above.
//
// RELEASE REQUIREMENT: every GitHub release (and the "test" pre-release) MUST attach a
// zip asset named exactly "mctde-Link.zip" containing d3d9.dll (and optionally a fresh
// mctde-link.ini / extras). The zip's files may sit at the zip root or inside a single
// top-level folder; the updater locates d3d9.dll automatically. The user's existing
// mctde-link.ini is never overwritten, so their settings survive the update.

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

// ============================================================
// Auto-updater
// ============================================================

// ------------------------------------------------------------
// Downloads any HTTPS URL to a file, following GitHub's
// release -> CDN redirects (WinHTTP follows HTTPS->HTTPS by
// default). Verifies the payload begins with the zip magic
// "PK" so a redirected HTML error page never gets installed.
// ------------------------------------------------------------
static bool DownloadUrlToFile(const wchar_t* url, const std::string& outPath)
{
    URL_COMPONENTS comps;
    ZeroMemory(&comps, sizeof(comps));
    comps.dwStructSize = sizeof(comps);

    wchar_t host[256] = { 0 };
    wchar_t urlPath[2048] = { 0 };
    comps.lpszHostName = host;
    comps.dwHostNameLength = ARRAYSIZE(host);
    comps.lpszUrlPath = urlPath;
    comps.dwUrlPathLength = ARRAYSIZE(urlPath);

    if (!WinHttpCrackUrl(url, 0, 0, &comps))
    {
        WriteLog("WinHttpCrackUrl failed for update URL.");
        return false;
    }

    HINTERNET hSession = WinHttpOpen(
        L"MCTDEUpdater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession)
    {
        WriteLog("WinHttpOpen failed (updater).");
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, comps.nPort, 0);

    if (!hConnect)
    {
        WriteLog("WinHttpConnect failed (updater).");
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (comps.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        urlPath,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );

    if (!hRequest)
    {
        WriteLog("WinHttpOpenRequest failed (updater).");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL))
    {
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

        if (statusCode == 200)
        {
            std::ofstream out(outPath.c_str(), std::ios::binary | std::ios::trunc);

            if (out.is_open())
            {
                DWORD size = 0;
                DWORD total = 0;
                bool firstChunk = true;
                bool looksLikeZip = false;

                do
                {
                    if (!WinHttpQueryDataAvailable(hRequest, &size))
                    {
                        WriteLog("WinHttpQueryDataAvailable failed (updater).");
                        break;
                    }

                    if (size == 0)
                    {
                        break;
                    }

                    std::vector<char> buffer(size);
                    DWORD downloaded = 0;

                    if (WinHttpReadData(hRequest, buffer.data(), size, &downloaded) && downloaded > 0)
                    {
                        if (firstChunk)
                        {
                            looksLikeZip = (downloaded >= 2 && buffer[0] == 'P' && buffer[1] == 'K');
                            firstChunk = false;
                        }

                        out.write(buffer.data(), downloaded);
                        total += downloaded;
                    }
                    else
                    {
                        WriteLog("WinHttpReadData failed (updater).");
                        break;
                    }
                }
                while (size > 0);

                out.close();

                if (looksLikeZip && total > 0)
                {
                    WriteLog("Downloaded update zip (" + std::to_string(total) + " bytes).");
                    ok = true;
                }
                else
                {
                    WriteLog("Downloaded update payload was not a valid zip; discarding.");
                    DeleteFileA(outPath.c_str());
                }
            }
            else
            {
                WriteLog("Could not open update file for writing.");
            }
        }
        else
        {
            WriteLog("Update download failed. Status code: " + std::to_string(statusCode));
        }
    }
    else
    {
        WriteLog("Update request send/receive failed.");
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return ok;
}

// ------------------------------------------------------------
// Wraps a string as a PowerShell single-quoted literal so paths
// with spaces (e.g. OneDrive) are passed safely. Single quotes
// inside are escaped by doubling, per PowerShell rules.
// ------------------------------------------------------------
static std::string PsSingleQuote(const std::string& text)
{
    std::string result = "'";

    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '\'')
        {
            result += "''";
        }
        else
        {
            result += text[i];
        }
    }

    result += "'";
    return result;
}

// ------------------------------------------------------------
// Downloads the latest release zip, then writes and launches a
// detached PowerShell helper that:
//   1. waits for this game process to fully exit (unlocking d3d9.dll),
//   2. extracts the zip and copies the new files into place
//      (preserving the user's existing mctde-link.ini),
//   3. relaunches Dark Souls,
//   4. cleans up the zip, staging folder, and itself.
// Returns true once the helper has been launched. The caller must
// then terminate the game so the locked d3d9.dll can be replaced.
// ------------------------------------------------------------
static bool LaunchUpdater()
{
    std::string installDir = GetDllDirectory();
    if (!installDir.empty() && installDir[installDir.size() - 1] != '\\' && installDir[installDir.size() - 1] != '/')
    {
        installDir += "\\";
    }

    std::string zipPath = installDir + "mctde-Link_update.zip";
    std::string stagingDir = installDir + "mctde-Link_update_tmp";
    std::string scriptPath = installDir + "mctde-Link_update.ps1";
    std::string updateLogPath = installDir + "mctde-Link_update.log";

    WriteLog("Downloading latest release from " "github.com/McRoodyPoo/mctde-Link/releases/latest" ".");

    if (!DownloadUrlToFile(MCTDE_LINK_RELEASE_ZIP_URL, zipPath))
    {
        WriteLog("Auto-update download failed.");
        return false;
    }

    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    DWORD gamePid = GetCurrentProcessId();

    std::ofstream script(scriptPath.c_str(), std::ios::trunc);
    if (!script.is_open())
    {
        WriteLog("Could not write updater helper script.");
        DeleteFileA(zipPath.c_str());
        return false;
    }

    script << "$ErrorActionPreference = 'Continue'\n";
    script << "$gamePid = " << gamePid << "\n";
    script << "$zip = " << PsSingleQuote(zipPath) << "\n";
    script << "$staging = " << PsSingleQuote(stagingDir) << "\n";
    script << "$install = " << PsSingleQuote(installDir) << "\n";
    script << "$exe = " << PsSingleQuote(std::string(exePath)) << "\n";
    script << "$self = " << PsSingleQuote(scriptPath) << "\n";
    script << "$ulog = " << PsSingleQuote(updateLogPath) << "\n";
    // Helper-side log so a failed swap/relaunch leaves a trail next to d3d9.dll.
    script << "function L($m){ try { Add-Content -LiteralPath $ulog -Value (\"[{0}] {1}\" -f (Get-Date).ToString('HH:mm:ss'), $m) } catch {} }\n";
    script << "L 'helper started.'\n";
    // 1. Wait for the game to close so d3d9.dll is no longer locked.
    script << "while (Get-Process -Id $gamePid -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }\n";
    script << "L 'game process exited; unlocking.'\n";
    script << "Start-Sleep -Milliseconds 750\n";
    // 2. Extract.
    script << "if (Test-Path $staging) { Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue }\n";
    script << "try { Expand-Archive -Path $zip -DestinationPath $staging -Force -ErrorAction Stop; L 'extracted zip.' } catch { L ('extract FAILED: ' + $_.Exception.Message) }\n";
    // Locate d3d9.dll anywhere in the extracted tree; its folder is the source root.
    script << "$dll = Get-ChildItem -Path $staging -Recurse -Filter d3d9.dll -ErrorAction SilentlyContinue | Select-Object -First 1\n";
    script << "if ($dll) {\n";
    script << "  $src = $dll.Directory.FullName\n";
    // 3. Copy files, preserving the user's existing ini. Retry the copy in case the
    //    file is briefly still locked right after the process exits.
    script << "  foreach ($f in (Get-ChildItem -Path $src -File)) {\n";
    script << "    if (($f.Name -ieq 'mctde-link.ini') -and (Test-Path (Join-Path $install $f.Name))) { continue }\n";
    script << "    $ok = $false\n";
    script << "    for ($i = 0; $i -lt 40; $i++) {\n";
    script << "      try { Copy-Item -LiteralPath $f.FullName -Destination $install -Force -ErrorAction Stop; $ok = $true; break }\n";
    script << "      catch { Start-Sleep -Milliseconds 500 }\n";
    script << "    }\n";
    script << "    L ('copied ' + $f.Name + ' = ' + $ok)\n";
    script << "  }\n";
    // Copy any subfolders shipped in the zip (e.g. a chainload folder) as-is.
    script << "  foreach ($d in (Get-ChildItem -Path $src -Directory)) { Copy-Item -LiteralPath $d.FullName -Destination $install -Recurse -Force -ErrorAction SilentlyContinue }\n";
    script << "} else { L 'no d3d9.dll found in the downloaded zip!' }\n";
    // 4. Clean up and relaunch.
    script << "Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue\n";
    script << "Remove-Item -Force $zip -ErrorAction SilentlyContinue\n";
    // Relaunch the LAUNCHER, not the game: this helper inherited the game's environment
    // (MCTDE_VIA_LAUNCHER=1 when started via the launcher), and Start-Process would pass that
    // flag to a relaunched game, making the launcher guard let it run directly instead of
    // reopening the launcher. Mirror the guard: prefer mctde_launcher.exe when it's present and
    // RequireLauncher isn't 0; otherwise fall back to relaunching the game exe.
    script << "$launcher = Join-Path $install 'mctde_launcher.exe'\n";
    script << "$ini = Join-Path $install 'mctde-link.ini'\n";
    script << "$useLauncher = (Test-Path $launcher)\n";
    script << "if ($useLauncher -and (Test-Path $ini) -and ((Get-Content -LiteralPath $ini -ErrorAction SilentlyContinue) -match '^\\s*RequireLauncher\\s*=\\s*0')) { $useLauncher = $false }\n";
    script << "if ($useLauncher) { try { Start-Process -FilePath $launcher -WorkingDirectory $install; L 'relaunched launcher.' } catch { L ('launcher relaunch FAILED: ' + $_.Exception.Message) } }\n";
    script << "elseif (Test-Path $exe) { try { Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe); L 'relaunched game.' } catch { L ('relaunch FAILED: ' + $_.Exception.Message) } }\n";
    script << "else { L 'nothing found to relaunch.' }\n";
    script << "L 'helper done.'\n";
    script << "Remove-Item -Force $self -ErrorAction SilentlyContinue\n";

    script.close();

    std::string command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"";
    command += scriptPath;
    command += "\"";

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // The game often runs inside a Job object (Steam / a launcher) with kill-on-close.
    // If we leave the helper inside that job, terminating the game tears the helper down
    // with it before it can swap the DLL. CREATE_BREAKAWAY_FROM_JOB pulls the helper out
    // of the job so it survives. CREATE_NO_WINDOW gives it a hidden console.
    //
    // Do NOT add DETACHED_PROCESS: combined with CREATE_NO_WINDOW it makes powershell.exe
    // launch but silently refuse to run its -File script (verified empirically). Breakaway
    // -- not detachment -- is what keeps the helper alive past the game's death.
    //
    // If the job forbids breakaway, CreateProcess fails with ERROR_ACCESS_DENIED -- fall
    // back to a plain CREATE_NO_WINDOW launch (still correct for the no-job case).
    // CreateProcess can write to its command-line buffer, so each attempt gets its own copy.
    std::string cmdBreakaway = command;
    BOOL launched = CreateProcessA(
        NULL,
        &cmdBreakaway[0],
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!launched)
    {
        DWORD breakawayErr = GetLastError();
        WriteLog("Breakaway launch failed (error " + std::to_string(breakawayErr) + "); retrying without breakaway.");

        std::string cmdPlain = command;
        ZeroMemory(&pi, sizeof(pi));
        launched = CreateProcessA(
            NULL,
            &cmdPlain[0],
            NULL,
            NULL,
            FALSE,
            CREATE_NO_WINDOW,
            NULL,
            NULL,
            &si,
            &pi
        );

        if (!launched)
        {
            WriteLog("Failed to launch updater helper. Error code: " + std::to_string(GetLastError()));
            DeleteFileA(zipPath.c_str());
            DeleteFileA(scriptPath.c_str());
            return false;
        }

        WriteLog("Updater helper launched (no breakaway).");
    }
    else
    {
        WriteLog("Updater helper launched (broke away from job).");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return true;
}

// ============================================================
// Launcher fetch (consent + HTTPS + verify)
// ============================================================

// Downloads an HTTPS URL fully into memory (follows GitHub's release->CDN redirects).
static bool DownloadToString(const wchar_t* url, std::string& outData)
{
    outData.clear();

    URL_COMPONENTS comps;
    ZeroMemory(&comps, sizeof(comps));
    comps.dwStructSize = sizeof(comps);
    wchar_t host[256] = { 0 };
    wchar_t urlPath[2048] = { 0 };
    comps.lpszHostName = host;       comps.dwHostNameLength = ARRAYSIZE(host);
    comps.lpszUrlPath = urlPath;     comps.dwUrlPathLength = ARRAYSIZE(urlPath);

    if (!WinHttpCrackUrl(url, 0, 0, &comps))
        return false;

    HINTERNET hSession = WinHttpOpen(L"MCTDELauncherFetch/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, comps.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD flags = (comps.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = false;
    // NOTE: default cert validation is left ON (we never set WINHTTP_OPTION_SECURITY_FLAGS to
    // ignore errors), so HTTPS authenticity of github.com is the primary trust anchor.
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL))
    {
        DWORD status = 0, ssz = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &ssz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200)
        {
            DWORD size = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &size)) break;
                if (size == 0) break;
                std::vector<char> buf(size);
                DWORD got = 0;
                if (WinHttpReadData(hRequest, buf.data(), size, &got) && got > 0)
                    outData.append(buf.data(), got);
                else
                    break;
            } while (size > 0);
            ok = !outData.empty();
        }
        else
        {
            WriteLog("Launcher fetch HTTP status " + std::to_string(status));
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// SHA-256 of a buffer as lowercase hex (BCrypt). "" on failure.
static std::string Sha256Hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        return "";

    DWORD hashLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0);
    std::vector<UCHAR> hash(hashLen);

    BCRYPT_HASH_HANDLE h = NULL;
    std::string out;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0)
    {
        if (BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
            BCryptFinishHash(h, hash.data(), hashLen, 0) == 0)
        {
            static const char* hx = "0123456789abcdef";
            out.reserve(hashLen * 2);
            for (UCHAR b : hash) { out += hx[b >> 4]; out += hx[b & 0xF]; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// If the launcher is missing (and the game wasn't started by it), offer to download it from
// the official GitHub release over HTTPS, verify it, install it, and open it. Consent-gated.
static void OfferLauncherInstall()
{
    char env[8] = { 0 };
    if (GetEnvironmentVariableA("MCTDE_VIA_LAUNCHER", env, sizeof(env)) > 0 && env[0] == '1')
        return;   // started by the launcher already

    std::string iniPath = GetVersionIniPath();
    if (GetPrivateProfileIntA("Launcher", "RequireLauncher", 1, iniPath.c_str()) == 0)
        return;   // feature disabled

    std::string dir = GetDllDirectory();
    if (!dir.empty() && dir[dir.size() - 1] != '\\' && dir[dir.size() - 1] != '/') dir += "\\";
    std::string launcherPath = dir + "mctde_launcher.exe";

    DWORD attr = GetFileAttributesA(launcherPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;   // already installed -- the launcher guard handles enforcement

    int r = MessageBoxA(NULL,
        "The mctde launcher isn't installed yet.\n\n"
        "mctde-Link uses a small launcher (mctde_launcher.exe) to apply DSFix and "
        "PhantomUnleashed options. Download and install it from the official source\n"
        "github.com/McRoodyPoo/mctde-Launcher (over HTTPS)?\n\n"
        "Yes  -  Download, verify, install, and open it. Dark Souls will close.\n"
        "No   -  Continue without it this time.",
        "Install the mctde launcher?", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (r != IDYES) { WriteLog("User declined launcher install."); return; }

    WriteLog("Downloading launcher from github.com/McRoodyPoo/mctde-Launcher.");
    std::string bytes;
    if (!DownloadToString(MCTDE_LAUNCHER_EXE_URL, bytes) ||
        bytes.size() < 20000 || bytes[0] != 'M' || bytes[1] != 'Z')   // must be a real PE
    {
        WriteLog("Launcher download failed or payload was not a valid .exe.");
        MessageBoxA(NULL,
            "Couldn't download the launcher.\n\nThe releases page will open so you can grab it manually.",
            "mctde launcher", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        ShellExecuteA(NULL, "open", MCTDE_LAUNCHER_RELEASES_URL, NULL, NULL, SW_SHOWNORMAL);
        return;   // leave the game running -- never strand the player
    }

    // Strict integrity check when a .sha256 sidecar is published alongside the exe.
    std::string sidecar;
    if (DownloadToString(MCTDE_LAUNCHER_SHA256_URL, sidecar))
    {
        std::string want = Trim(sidecar);
        size_t sp = want.find_first_of(" \t\r\n");
        if (sp != std::string::npos) want = want.substr(0, sp);
        for (size_t i = 0; i < want.size(); ++i) want[i] = (char)tolower((unsigned char)want[i]);
        std::string got = Sha256Hex(bytes);
        if (!want.empty() && !got.empty() && want != got)
        {
            WriteLog("Launcher SHA-256 mismatch (want " + want + ", got " + got + "); discarding.");
            MessageBoxA(NULL,
                "The downloaded launcher failed its integrity check and was discarded.\n\n"
                "Nothing was installed. Please download it manually.",
                "mctde launcher", MB_OK | MB_ICONERROR | MB_TOPMOST);
            ShellExecuteA(NULL, "open", MCTDE_LAUNCHER_RELEASES_URL, NULL, NULL, SW_SHOWNORMAL);
            return;
        }
        WriteLog("Launcher SHA-256 verified.");
    }
    else
    {
        WriteLog("No SHA-256 sidecar published; relying on HTTPS authenticity + PE check.");
    }

    std::ofstream out(launcherPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { WriteLog("Could not write launcher to disk."); return; }
    out.write(bytes.data(), (std::streamsize)bytes.size());
    out.close();
    WriteLog("Launcher installed (" + std::to_string(bytes.size()) + " bytes). Opening it and closing the game.");

    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::string cmd = "\"" + launcherPath + "\"";
    std::vector<char> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(0);
    std::string wd = dir;
    if (!wd.empty() && (wd[wd.size() - 1] == '\\' || wd[wd.size() - 1] == '/')) wd.erase(wd.size() - 1);
    if (CreateProcessA(launcherPath.c_str(), cmdbuf.data(), NULL, NULL, FALSE, 0, NULL, wd.c_str(), &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Sleep(300);
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

// ------------------------------------------------------------
// This runs after the DLL loads.
// Do NOT do internet stuff directly inside DllMain.
// ------------------------------------------------------------
DWORD WINAPI VersionCheckThread(LPVOID)
{
    WriteLog("----------------------------------------");
    WriteLog("Version checker started.");
    WriteLog("Channel: " MCTDE_LINK_CHANNEL_LABEL);

    // If the launcher is missing, offer to fetch it first (may close the game and open it).
    OfferLauncherInstall();

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

    // mctde takes priority. It lives on NexusMods (which can't be scripted from a
    // mod DLL), so it keeps the manual open-the-page flow rather than auto-installing.
    if (mctdeOutOfDate)
    {
        WriteLog("mctde update available.");

        std::string popupMessage =
            "mctde is out of date. Open the download page?\n\n"
            "Installed mctde version: " + std::string(CURRENT_MCTDE_VERSION) + "\n"
            "Latest mctde version: " + latestVersions.mctde + "\n\n"
            "Dark Souls will close either way so you can install the update.";

        int result = MessageBoxA(
            NULL,
            popupMessage.c_str(),
            "mctde Update Required",
            MB_YESNO | MB_ICONWARNING | MB_TOPMOST
        );

        if (result == IDYES)
        {
            WriteLog("User chose to update mctde. Opening NexusMods page, then closing Dark Souls.");
            ShellExecuteA(NULL, "open", MCTDE_DOWNLOAD_URL, NULL, NULL, SW_SHOWNORMAL);
            Sleep(750); // give the browser a moment to launch before we exit
        }
        else
        {
            WriteLog("User declined mctde update. Closing Dark Souls.");
        }

        TerminateProcess(GetCurrentProcess(), 0);
        ExitProcess(0);
        return 0;
    }

    // mctde-Link auto-update.
    WriteLog("mctde-link update available.");

    std::string popupMessage =
        "A new version of mctde-Link is available.\n\n"
        "Installed: " + std::string(CURRENT_MCTDE_LINK_VERSION) + "\n"
        "Latest: " + latestVersions.mctdeLink + "\n\n"
        "Yes  -  Download and install the update automatically. Dark Souls will close and reopen.\n\n"
        "No  -  Close Dark Souls and open the releases page so you can update manually.";

#ifdef MCTDE_LINK_TEST_CHANNEL
    const char* updateTitle = "[TEST] mctde-Link Update Available";
#else
    const char* updateTitle = "mctde-Link Update Available";
#endif

    int result = MessageBoxA(
        NULL,
        popupMessage.c_str(),
        updateTitle,
        MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST
    );

    if (result == IDYES)
    {
        WriteLog("User chose auto-update.");

        if (LaunchUpdater())
        {
            WriteLog("Auto-update staged. Closing Dark Souls so the new d3d9.dll can be installed.");
        }
        else
        {
            // Download or helper launch failed -- fall back to the manual page so the
            // user is never left stuck on an out-of-date build.
            WriteLog("Auto-update failed. Falling back to the releases page.");
            MessageBoxA(
                NULL,
                "The automatic update could not be downloaded.\n\n"
                "The releases page will open so you can update manually.",
                "mctde-Link Update",
                MB_OK | MB_ICONWARNING | MB_TOPMOST
            );
            ShellExecuteA(NULL, "open", MCTDE_LINK_RELEASES_LATEST_URL, NULL, NULL, SW_SHOWNORMAL);
            Sleep(750);
        }
    }
    else
    {
        WriteLog("User declined auto-update. Opening releases page and closing Dark Souls.");
        ShellExecuteA(NULL, "open", MCTDE_LINK_RELEASES_LATEST_URL, NULL, NULL, SW_SHOWNORMAL);
        Sleep(750);
    }

    // Close Dark Souls so the locked d3d9.dll can be replaced (by the helper on Yes,
    // or by the user manually on No / fallback).
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
