#include "pch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include "D3DOverlay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <ctype.h>

#pragma comment(lib, "ws2_32.lib")

// MCTDE NetOverlay v80 narrow centered gutter
// Strategy:
//   1. WORLD actor rows are authoritative. If a player body/slot is not live in WorldChrBase, it is not displayed.
//   2. Steam P2P packets are used only to learn SteamID -> playerNo from the live packet stream.
//   3. Steam node/connection tables are used only to attach name + ping to that SteamID.
// This intentionally keeps WORLD rows authoritative, shows cached connection-table ping with name fallback plus short-lived ping cache, polls HP at 60 Hz from Ashley/PTDE ChrIns offsets, and lingers the 1HP marker, maps the local Steam node to the local player number, reads names from playerParam, distinguishes red summons from invaders when the world row exposes the marker, and uses Ashley PlayerParam InvadeType as the primary entry-route classifier, using Vow_Type only as a bounded fallback for Warrior of Sunlight white summons and blue-route disambiguation, and commits a name/role text style as soon as a confident entry-route style is observed, treats InvadeType=NormalWhite as the authoritative white-summon route even when the phantom visible bucket looks blue, then keeps that style by playerNo/SteamID/displayed-name only while that phantom row remains visible; committed phantom styles are cleared immediately when the phantom drops/loading starts; true-ping rows no longer show a (T) suffix, cached fallback rows can show (C), the local row shows \[T]/ instead of ---MS, and the ping/local marker is centered in a narrower HP/name gutter while the name starts at a stable, separated x-position.

static const char* LOG_FILE_NAME = "MCTDE_NetOverlay";
static const char* INI_FILE_NAME = "mctde-link.ini";

static const COLORREF OVERLAY_TRANSPARENT_KEY = RGB(255, 0, 255);

static char g_logPath[MAX_PATH];
static char g_iniPath[MAX_PATH];
static bool g_loggingEnabled = false;

static void SafeLstrcpynA(char* dst, const char* src, int count)
{
    LPSTR ret = lstrcpynA(dst, src ? src : "", count);
    UNREFERENCED_PARAMETER(ret);
}

// RVAs from DARKSOULS.exe base.
static const uintptr_t OFF_STEAM_NODE_LIST = 0xF62DCC;
static const uintptr_t OFF_STEAM_NODE_COUNT = 0xF62DD0;
static const uintptr_t OFF_SESSION_MANAGER = 0xF62CC0;

static const uintptr_t OFF_SESSION_P2P_SYSTEM = 0x64;
static const uintptr_t OFF_P2P_CONNECTION_LIST = 0x54;
static const uintptr_t OFF_LISTNODE_CONNECTION = 0x08;

static const uintptr_t OFF_CONNECTION_STATUS = 0x08;
static const uintptr_t OFF_CONNECTION_CID = 0x10;
static const uintptr_t OFF_CONNECTION_STEAMID64 = 0x170;
static const uintptr_t OFF_CONNECTION_PING = 0x178;

// Ashley CT WorldChrBase AOB. The first dword at the match is the static address CE registers as WorldChrBase.
static uintptr_t g_worldChrBaseAddress = 0;
static bool g_triedWorldChrBaseScan = false;

static const uintptr_t OFF_WORLDCHR_SELF = 0x3C;
static const uintptr_t OFF_CHR_MP_ROOT = 0x0C;
static const uintptr_t OFF_MPCHR1 = 0x20;
static const uintptr_t OFF_MPCHR2 = 0x40;
static const uintptr_t OFF_MPCHR3 = 0x60;
static const uintptr_t OFF_CHR_PLAYER_PARAM = 0x414;
static const uintptr_t OFF_CHR_PHANTOM_DATA = 0x658;
// DS-Gadget exposes these as CharData1 ChrType/TeamType. Ashley table paths read
// the same live actor fields at chr + 0x70 / chr + 0x74. Use them as a live
// visual override so cheat/tool changes show up immediately in the overlay.
static const uintptr_t OFF_CHR_CHR_TYPE = 0x70;
static const uintptr_t OFF_CHR_TEAM_TYPE = 0x74;
static const uintptr_t OFF_PLAYERPARAM_PLAYERNO = 0x08;
// Ashley table: PlayerParam -> VowData -> Vow_Type is playerParam + 0x10B.
// Ashley table: PlayerParam -> InvadeType is playerParam + 0x110.
static const uintptr_t OFF_PLAYERPARAM_CHR_TYPE = 0x9C;
static const uintptr_t OFF_PLAYERPARAM_VOW_TYPE = 0x10B;
static const uintptr_t OFF_PLAYERPARAM_INVADE_TYPE = 0x110;
static const uintptr_t OFF_PLAYERPARAM_NAME_WIDE = 0xA0;
static const uintptr_t OFF_PHANTOM_IS_PHANTOM = 0x90;
static const uintptr_t OFF_PHANTOM_TYPE = 0x91;
// Observed in v19/v20 logs: for red phantoms, byte +0x96 is 3 for red-sign summons and 0 for true invaders.
// If this byte is anything else, keep the label conservative as Red.
static const uintptr_t OFF_PHANTOM_RED_KIND = 0x96;

// Live HP offsets from the PTDE/Ashley-style table path, read directly from the live WorldChr actor.
// Confirmed in the PTDE CE script / Ashley table family:
//   chr + 0x2D4 = current HP
//   chr + 0x2D8 = max HP
// Defaults can be overridden in the INI with DECIMAL values:
//   [HP]
//   Enabled=1
//   PollMs=16
//   CurrentOffset=724  ; 0x2D4
//   MaxOffset=728      ; 0x2D8
//   AutoScan=0
static const int DEFAULT_HP_CURRENT_OFFSET = 0x2D4;
static const int DEFAULT_HP_MAX_OFFSET = 0x2D8;

static const uintptr_t OFF_NODE_STEAM_PLAYER_DATA = 0x0C;
static const uintptr_t OFF_STEAM_PLAYER_ONLINE_ID_DATA = 0x0C;
static const uintptr_t OFF_STEAM_PLAYER_NAME = 0x30;
static const uintptr_t OFF_STEAM_ONLINE_ID_STRING = 0x30;

// Ghidra VA 0072BB40 = SteamSessionLight receive handler.
// Hook point is success path after SteamNetworking()->ReadP2PPacket filled bytesRead and remote SteamID.
// VA 0072BBBA bytes observed: 8B 7C 24 14 83 FF 02
static const uintptr_t OFF_STEAM_P2P_READ_SUCCESS = 0x32BBBA;

// Ghidra VA 00E688E0 = ChrIns/current HP setter. The specific writes below are inside that function.
// These are used only as detectors; the original write still executes through the trampoline.
static const uintptr_t OFF_HP_WRITE_EAX_00E6891D = 0x00A6891D; // mov [ebp+2D4], eax
static const uintptr_t OFF_HP_WRITE_EBX_00E68960 = 0x00A68960; // mov [ebp+2D4], ebx
static const uintptr_t OFF_HP_WRITE_EBX_00E68981 = 0x00A68981; // mov [ebp+2D4], ebx
static const uintptr_t OFF_HP_WRITE_EBX_00E68991 = 0x00A68991; // mov [ebp+2D4], ebx

// Suppress the blocking "<?leaveName?> has returned home" / "sent home" request popups.
// Absolute Ghidra addresses:
//   FUN_00ea1bf0 -> RVA 0x00AA1BF0 = regular leave/returned-home caller
//   FUN_00ea2510 -> RVA 0x00AA2510 = session add/join caller; calls FUN_00d74950 at 00EA26F4
//   FUN_00d74950 -> RVA 0x00974950 = ActionEventInfo/RecallMenuEvent display helper
//   FUN_00d4c190 -> RVA 0x0094C190 = request/gen-dialog popup opener
//
// The "Phantom <?leaveName?> has returned home" path at 00EA2DD0 does NOT call 00D74950.
// It does:
//   MOV EAX, 13Fh            ; leaveName tag
//   CALL 00D10CD0            ; fill <?leaveName?>
//   PUSH 1Eh
//   MOV ECX, 01312EA9h       ; message id 20000425
//   CALL 00D4C190            ; blocking popup
//   ADD ESP, 4
//
// 00D4C190 is not stdcall. It takes msgId in ECX and mode/type on the stack, then returns with plain RET; callers clean the stack.
static const uintptr_t OFF_RETURNED_HOME_POPUP_CALLER_00EA1BF0 = 0x00AA1BF0;
static const uintptr_t OFF_RETURNED_HOME_POPUP_CALLER_00EA2510 = 0x00AA2510;
static const uintptr_t OFF_RECALL_MENU_EVENT_00D74950 = 0x00974950;
static const uintptr_t OFF_REQUEST_POPUP_00D4C190 = 0x0094C190;
static const int RETURNED_HOME_CALLER_SCAN_LEN_00EA1BF0 = 0x100;
static const int RETURNED_HOME_CALLER_SCAN_LEN_00EA2510 = 0x240;

struct PingInfo
{
    int status = 0;
    int cid = 0;
    int ping = -1;
    uintptr_t connection = 0;
};

struct CachedPingInfo
{
    PingInfo ping = {};
    ULONGLONG tick = 0;
};

struct TruePingPeerInfo
{
    bool handshaken = false;
    int rttMs = -1;
    int pingMs = -1;          // displayed ping value after display filtering
    int rawPingMs = -1;       // latest direct RTT/2 sample
    int displayPingMs = -1;   // smoothing accumulator
    int sampleCount = 0;
    int sampleRing[16] = {};  // recent raw ping samples used by floor/best display mode
    int sampleRingIndex = 0;
    int sampleRingCount = 0;
    ULONGLONG lastSeenTick = 0;
    ULONGLONG lastHelloTick = 0;
    ULONGLONG lastPingTick = 0;
    uint32_t lastNonce = 0;
    uint64_t lastPingQpc = 0;
};

#pragma pack(push, 1)
struct MctdePingPacket
{
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t nonce;
    uint64_t senderSteamId;
    uint64_t qpc;
};
#pragma pack(pop)

struct SteamNodeInfo
{
    std::string name;
    std::string steamIdText;
    uint64_t steamId = 0;
    bool isLocal = false;
    bool hasPing = false;
    PingInfo ping = {};
};

struct WorldActorRow
{
    const char* slotName = "";
    int slotIndex = -1;
    bool valid = false;
    uintptr_t chr = 0;
    uintptr_t playerParam = 0;
    uintptr_t phantomData = 0;
    int playerNo = -1;
    int isPhantom = 0;
    int phantomType = -1;
    int redKind = -1;
    int invadeType = -1;
    int vowType = -1;
    int chrType = -1;
    int teamType = -1;
    int playerParamChrType = -1;
    bool hasHp = false;
    int hp = -1;
    int hpMax = -1;
    int hpCurrentOffset = -1;
    int hpMaxOffset = -1;
    std::string inGameName;
};

struct PlayerIdentity
{
    int playerNo = -1;
    uint64_t steamId = 0;
    ULONGLONG lastSeenTick = 0;
    LONG learnCount = 0;
};

struct OverlayRow
{
    const char* slotName = "";
    int slotIndex = -1;
    uintptr_t chr = 0;
    int playerNo = -1;
    bool isLocal = false;
    int isPhantom = 0;
    int phantomType = -1;
    int redKind = -1;
    int invadeType = -1;
    int vowType = -1;
    int chrType = -1;
    int teamType = -1;
    int playerParamChrType = -1;
    uint64_t steamId = 0;
    std::string name;
    std::string worldName;
    bool hasName = false;
    bool hasPing = false;
    int status = 0;
    int cid = 0;
    int ping = -1;
    bool hasTruePing = false;
    int truePing = -1;
    int trueRtt = -1;
    bool hasHp = false;
    int hp = -1;
    int hpMax = -1;
    int hpCurrentOffset = -1;
    int hpMaxOffset = -1;
    ULONGLONG hpOneUntilTick = 0;
    uintptr_t connection = 0;
    ULONGLONG learnedAgeMs = 0;
    int stickyRoute = 0;
    ULONGLONG stickyRouteAgeMs = 0;
    bool disconnected = false; // synthetic placeholder: player left the session without dying
};

static HINSTANCE g_hInstance = NULL;
static HWND g_overlayWnd = NULL;
static HWND g_gameWnd = NULL;
static HWND g_overlayOwnerWnd = NULL;
static volatile bool g_running = true;
// Anti-zombie watchdog: when the game window is destroyed (game closing), force a clean
// process exit so the overlay's background threads can't keep DARKSOULS.exe alive.
// [Settings] ExitWithGame=0 disables it.
static bool g_exitWithGame = true;

static CRITICAL_SECTION g_rowsLock;
static bool g_rowsLockReady = false;
static std::vector<OverlayRow> g_rows;
static std::vector<WorldActorRow> g_worldRows;
static std::unordered_map<uintptr_t, ULONGLONG> g_oneHpUntilByChr;
static std::unordered_map<uint64_t, std::string> g_nameCacheBySteamId;
static uint64_t g_localSteamId = 0;
static std::unordered_map<uint64_t, CachedPingInfo> g_pingCacheBySteamId;
static std::unordered_map<std::string, CachedPingInfo> g_pingCacheByName;
static const DWORD PING_CACHE_TTL_MS = 0; // 0 = keep last known ping forever until replaced by a new sample.
static std::unordered_map<uint64_t, ULONGLONG> g_recentP2PRemoteSteamIds;
static const DWORD RECENT_P2P_REMOTE_TTL_MS = 60000;

static CRITICAL_SECTION g_truePingLock;
static bool g_truePingLockReady = false;
static std::unordered_map<uint64_t, TruePingPeerInfo> g_truePingBySteamId;
static std::unordered_map<uint64_t, std::string> g_truePingKnownRemoteNames;
static ULONGLONG g_truePingKnownRemoteTick = 0;
static volatile LONG g_truePingSeq = 0;
static volatile LONG g_truePingNonce = 0;
static bool g_truePingEnabled = true;
static bool g_truePingDebug = false;
static bool g_truePingVerbose = false;
static bool g_truePingPreferOverlay = true;
static bool g_truePingShowSourceMarker = true;
static int g_truePingChannel = 63;
static bool g_truePingAllowGameChannel = false;
static bool g_truePingSendEnabled = true;
static bool g_truePingReceiveEnabled = true;
static int g_truePingSendMs = 1000;
static int g_truePingHelloMs = 2000;
static int g_truePingStaleMs = 4000;
static int g_truePingSendType = 2; // MPChan used 2 with channel 1. Keep staged/opt-in.
static bool g_truePingSmoothDisplay = true;
static int g_truePingSmoothWeight = 4;
// DisplayMode: 0 = latest raw RTT/2, 1 = EWMA smoothing, 2 = recent floor/best sample.
// Floor mode is default because same-PC/LAN tests often bounce between one and two
// Windows scheduler slices (7/15, 8/16). The lower recent sample is usually the
// real network floor; the higher sample is polling/scheduling delay.
static int g_truePingDisplayMode = 2;
static int g_truePingBestWindow = 8;
static int g_truePingPollSleepMs = 1;
static bool g_truePingUseHighResTimer = true;
static bool g_truePingTimerPeriodSet = false;
typedef UINT(WINAPI* TimePeriodFn)(UINT);
static TimePeriodFn g_timeBeginPeriodFn = NULL;
static TimePeriodFn g_timeEndPeriodFn = NULL;

static CRITICAL_SECTION g_identityLock;
static bool g_identityLockReady = false;
static const int MAX_TRACKED_PLAYER_NO = 32;
static PlayerIdentity g_identityByPlayerNo[MAX_TRACKED_PLAYER_NO];

static int g_refreshMs = 1000;
// Render backend: 0 = legacy GDI layered window, 1 = in-frame Direct3D9 overlay.
static int g_renderBackend = 0;
static int g_d3dSubmitMs = 66; // how often the D3D backend rasterizes + submits a new bitmap
static int g_repaintMs = 66;   // GDI-window repaint interval (lower = smoother but more DWM cost)

// Cached GDI window resources/state so we stop recreating them every paint.
static HFONT g_winSmallFont = NULL;
static HFONT g_winHpFont = NULL;
static int   g_winFontHeightCached = -1;
static int   g_winHpFontHeightCached = -1;
static char  g_winFontFaceCached[64] = "";
static FILETIME g_iniLastWrite = { 0, 0 };
static bool  g_iniLastWriteValid = false;
static bool  g_overlayStyleApplied = false;
static RECT  g_lastOverlayRect = { -1, -1, -1, -1 };
static int g_paddingX = 18;
static int g_paddingY = 18;
static int g_fontHeight = 16;
static int g_hpFontHeight = 32;
static int g_lineHeight = 36;
static int g_markerGutterExtra = 24; // extra pixels around the MS/local marker between HP and name.
static char g_fontFace[LF_FACESIZE] = "Tahoma";
static bool g_hideLocal = false;
static bool g_showHeader = true;
static bool g_forceTopmost = false;
static bool g_dumpOverlayData = false;
static bool g_debugP2PBridge = true;
static bool g_showUnknownWorldRows = true;
static bool g_showPing = true;
static bool g_showHp = true;
// Display-only element toggles ([Overlay] section). These hide elements in the overlay
// without touching the underlying subsystems: [HP] Enabled still gates HP polling, so
// ShowHp=0 only removes the HP column from the drawn overlay.
static bool g_showHpField = true;     // [Overlay] ShowHp
static bool g_showName = true;        // [Overlay] ShowName
static bool g_showLocalMarker = true; // [Overlay] ShowLocalMarker
static bool g_showDisconnected = true;// [Overlay] ShowDisconnected: keep a "Disconnected" row
                                      // for a player who leaves without dying, until replaced.
// Master overlay visibility, toggled live by the hotkey. Not persisted to the INI.
static volatile bool g_overlayVisible = true;
// Virtual-key code for the master toggle. [Overlay] ToggleKey (default VK_F3 = 0x72).
static int g_toggleKey = VK_F3;
// Optional modifier that must be held with the toggle key (0 = none). [Overlay] ToggleModifier.
// Defaults to Shift so the bind is Shift+F3, which avoids DSFix's hotkeys.
static int g_toggleModifier = VK_SHIFT;
static bool g_hpAutoScan = false;
static int g_hpPollMs = 1;
static bool g_hpDetectInstantRefill = true;
static int g_hpInstantRefillMinGain = 100;
static int g_hpCurrentOffset = DEFAULT_HP_CURRENT_OFFSET;
static int g_hpMaxOffset = DEFAULT_HP_MAX_OFFSET;
static DWORD g_hpOneLingerMs = 500;
static DWORD g_identityTtlMs = 120000;

// ------------------------------------------------------------
// Local OBS browser-source WebSocket server.
//
// INI:
//   [WebSocket]
//   Enabled=1
//   Port=39876
//   SendMs=33
//
// OBS Browser Source URL:
//   http://127.0.0.1:39876/overlay.html
//
// If Port is changed in mctde-link.ini, use:
//   http://127.0.0.1:<Port>/overlay.html
//
// This is a local WebSocket/browser-source server for OBS. It is not an
// outbound webhook and does not send data to the internet.
// ------------------------------------------------------------
static bool g_webSocketEnabled = true;
static int g_webSocketPort = 39876;
static int g_webSocketSendMs = 33;
static SOCKET g_webSocketListenSocket = INVALID_SOCKET;
static CRITICAL_SECTION g_webSocketClientsLock;
static bool g_webSocketClientsLockReady = false;
static std::vector<SOCKET> g_webSocketClients;
static volatile LONG g_webSocketClientCount = 0;
static volatile LONG g_webSocketSendSeq = 0;

static volatile LONG g_dumpCounter = 0;
static volatile LONG g_configReloadCounter = 0;
static volatile LONG g_p2pRxSeq = 0;
static void* g_steamP2PReadSuccessTrampoline = NULL;
static void* g_hpWriteEax_00E6891D_Trampoline = NULL;
static void* g_hpWriteEbx_00E68960_Trampoline = NULL;
static void* g_hpWriteEbx_00E68981_Trampoline = NULL;
static void* g_hpWriteEbx_00E68991_Trampoline = NULL;
static void* g_recallMenuEvent_00D74950_Trampoline = NULL;
static void* g_requestPopup_00D4C190_Trampoline = NULL;

static bool g_hooksInstalled = false;

typedef void* (__cdecl* SteamNetworkingThunkFn)();
typedef bool(__thiscall* SteamSendP2PPacketFn)(void* self, uint64_t steamIDRemote, const void* pubData, uint32_t cubData, int eP2PSendType, int nChannel);
typedef bool(__thiscall* SteamIsP2PPacketAvailableFn)(void* self, uint32_t* pcubMsgSize, int nChannel);
typedef bool(__thiscall* SteamReadP2PPacketFn)(void* self, void* pubDest, uint32_t cubDest, uint32_t* pcubMsgSize, uint64_t* psteamIDRemote, int nChannel);

static void* g_steamNetworkingIface = NULL;
static SteamSendP2PPacketFn g_steamSendP2PPacket = NULL;
static SteamIsP2PPacketAvailableFn g_steamIsP2PPacketAvailable = NULL;
static SteamReadP2PPacketFn g_steamReadP2PPacket = NULL;
static LARGE_INTEGER g_qpcFrequency;

static const uint32_t MCTP_MAGIC = 0x5054434D; // 'MCTP' little-endian.
static const uint16_t MCTP_VERSION = 1;
static const uint16_t MCTP_HELLO = 1;
static const uint16_t MCTP_HELLO_ACK = 2;
static const uint16_t MCTP_PING = 3;
static const uint16_t MCTP_PONG = 4;

enum OverlayCorner
{
    CORNER_TOP_LEFT,
    CORNER_TOP_RIGHT,
    CORNER_BOTTOM_LEFT,
    CORNER_BOTTOM_RIGHT
};

static OverlayCorner g_corner = CORNER_TOP_LEFT;
static char g_cornerText[64] = "top_left";

static uintptr_t ExeBase()
{
    return (uintptr_t)GetModuleHandleA(NULL);
}

static uintptr_t Rva(uintptr_t rva)
{
    return ExeBase() + rva;
}

static void WriteLogLine(const char* text)
{
    if (!g_loggingEnabled)
        return;

    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, text, (DWORD)lstrlenA(text), &written, NULL);
        WriteFile(h, "\r\n", 2, &written, NULL);
        CloseHandle(h);
    }
}

static void WriteLogf(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    WriteLogLine(buf);
}

static void BuildPaths()
{
    char dllPath[MAX_PATH];
    ZeroMemory(dllPath, sizeof(dllPath));
    GetModuleFileNameA(g_hInstance, dllPath, MAX_PATH);

    char* lastSlash = strrchr(dllPath, '\\');
    if (lastSlash)
        *(lastSlash + 1) = 0;
    else
        lstrcpyA(dllPath, ".\\");

    lstrcpyA(g_logPath, dllPath);
    char pidPart[64];
    wsprintfA(pidPart, "%s_%lu.log", LOG_FILE_NAME, GetCurrentProcessId());
    lstrcatA(g_logPath, pidPart);

    lstrcpyA(g_iniPath, dllPath);
    lstrcatA(g_iniPath, INI_FILE_NAME);
}

static void DeleteOverlayLogsInDllDir()
{
    char dir[MAX_PATH];
    lstrcpyA(dir, g_logPath);

    char* lastSlash = strrchr(dir, '\\');
    if (lastSlash)
        *(lastSlash + 1) = 0;
    else
        lstrcpyA(dir, ".\\");

    char pattern[MAX_PATH];
    lstrcpyA(pattern, dir);
    lstrcatA(pattern, LOG_FILE_NAME);
    lstrcatA(pattern, "_*.log");

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            char path[MAX_PATH];
            lstrcpyA(path, dir);
            lstrcatA(path, data.cFileName);
            DeleteFileA(path);
        }
    } while (FindNextFileA(find, &data));

    FindClose(find);
}

static void LoadLoggingSetting()
{
    g_loggingEnabled = GetPrivateProfileIntA("Settings", "EnableLogging", 0, g_iniPath) != 0;
    if (!g_loggingEnabled)
        DeleteOverlayLogsInDllDir();
}

static void TrimString(char* s)
{
    if (!s)
        return;

    int len = lstrlenA(s);
    while (len > 0)
    {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            s[len - 1] = 0;
            len--;
        }
        else
            break;
    }

    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;

    if (start != s)
        MoveMemory(s, start, lstrlenA(start) + 1);

    char* semi = strchr(s, ';');
    if (semi)
    {
        *semi = 0;
        TrimString(s);
    }

    char* hash = strchr(s, '#');
    if (hash)
    {
        *hash = 0;
        TrimString(s);
    }
}

static const char* CornerToText()
{
    switch (g_corner)
    {
    case CORNER_TOP_LEFT: return "top_left";
    case CORNER_TOP_RIGHT: return "top_right";
    case CORNER_BOTTOM_LEFT: return "bottom_left";
    case CORNER_BOTTOM_RIGHT: return "bottom_right";
    default: return "unknown";
    }
}

template <typename T>
static bool SafeRead(uintptr_t address, T& out)
{
    __try
    {
        out = *(T*)address;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ZeroMemory(&out, sizeof(T));
        return false;
    }
}

static bool SafeReadBytes(uintptr_t address, void* out, size_t size)
{
    __try
    {
        memcpy(out, (void*)address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ZeroMemory(out, size);
        return false;
    }
}

static bool LooksLikePointer(uintptr_t p)
{
    if (p < 0x10000)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if (mbi.Protect & PAGE_NOACCESS)
        return false;

    if (mbi.Protect & PAGE_GUARD)
        return false;

    return true;
}

static std::string SafeReadAsciiString(uintptr_t address, int maxLen)
{
    if (!LooksLikePointer(address))
        return std::string();

    char buffer[256];
    ZeroMemory(buffer, sizeof(buffer));

    if (maxLen > 255)
        maxLen = 255;

    if (!SafeReadBytes(address, buffer, maxLen))
        return std::string();

    buffer[maxLen] = 0;

    for (int i = 0; i < maxLen; i++)
    {
        unsigned char c = (unsigned char)buffer[i];
        if (c == 0)
            break;

        if (c < 0x20 || c > 0x7E)
        {
            buffer[i] = 0;
            break;
        }
    }

    return std::string(buffer);
}

static std::string SafeReadWideString(uintptr_t address, int maxChars)
{
    // playerParam + 0xA0 behaves like a 16-bit little-endian name buffer.
    // This overlay still draws with TextOutA, so keep display names ASCII-safe.
    // Printable ASCII is preserved. Non-ASCII is replaced with '?'.
    // If a name contains no printable ASCII at all, return empty so the normal Unknown fallback is used.
    if (!LooksLikePointer(address))
        return std::string();

    wchar_t wbuffer[128];
    ZeroMemory(wbuffer, sizeof(wbuffer));

    if (maxChars > 127)
        maxChars = 127;

    if (!SafeReadBytes(address, wbuffer, maxChars * sizeof(wchar_t)))
        return std::string();

    char out[256];
    ZeroMemory(out, sizeof(out));

    int outPos = 0;
    bool hadPrintableAscii = false;

    for (int i = 0; i < maxChars && outPos < 255; i++)
    {
        wchar_t wc = wbuffer[i];

        if (wc == 0)
            break;

        // Stop on control garbage.
        if (wc < 0x20)
            break;

        // Printable ASCII range. This is safe for the current TextOutA path.
        if (wc >= 0x20 && wc <= 0x7E)
        {
            out[outPos++] = (char)wc;
            hadPrintableAscii = true;
        }
        else
        {
            // Censor/replace non-ASCII instead of mis-decoding it.
            out[outPos++] = '?';
        }
    }

    out[outPos] = 0;

    if (!hadPrintableAscii)
        return std::string();

    return std::string(out);
}

static std::string SafeReadAutoString(uintptr_t address, int maxChars)
{
    if (!LooksLikePointer(address))
        return std::string();

    unsigned char probe[8];
    ZeroMemory(probe, sizeof(probe));

    if (!SafeReadBytes(address, probe, sizeof(probe)))
        return std::string();

    if (probe[0] != 0 && probe[1] == 0)
        return SafeReadWideString(address, maxChars);

    return SafeReadAsciiString(address, maxChars);
}

static uint64_t ParseSteamIdTextHex(const std::string& text)
{
    if (text.empty())
        return 0;

    return _strtoui64(text.c_str(), NULL, 16);
}

static std::string NormalizeNameKey(const std::string& name)
{
    if (name.empty() || name == "Unknown" || name == "(unknown)")
        return std::string();

    std::string out = name;
    for (size_t i = 0; i < out.size(); i++)
    {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    }

    return out;
}

static bool IsFreshCachedPing(const CachedPingInfo& cached)
{
    if (cached.tick == 0)
        return false;

    // v37: do not expire cached pings.
    // If the Steam/session table stops exposing a fresh ping later, keep showing
    // the last known value instead of dropping back to ---MS.
    if (cached.ping.ping < 0)
        return false;

    return true;
}

static void RememberCachedPing(uint64_t steamId, const std::string& name, const PingInfo& ping)
{
    if (ping.ping < 0)
        return;

    CachedPingInfo cached;
    cached.ping = ping;
    cached.tick = GetTickCount64();

    if (steamId != 0)
        g_pingCacheBySteamId[steamId] = cached;

    std::string key = NormalizeNameKey(name);
    if (!key.empty())
        g_pingCacheByName[key] = cached;
}

static bool FindCachedPingBySteamId(uint64_t steamId, PingInfo& out)
{
    if (steamId == 0)
        return false;

    std::unordered_map<uint64_t, CachedPingInfo>::iterator it = g_pingCacheBySteamId.find(steamId);
    if (it == g_pingCacheBySteamId.end())
        return false;

    if (!IsFreshCachedPing(it->second))
        return false;

    out = it->second.ping;
    return true;
}

static bool FindCachedPingByName(const std::string& name, PingInfo& out)
{
    std::string key = NormalizeNameKey(name);
    if (key.empty())
        return false;

    std::unordered_map<std::string, CachedPingInfo>::iterator it = g_pingCacheByName.find(key);
    if (it == g_pingCacheByName.end())
        return false;

    if (!IsFreshCachedPing(it->second))
        return false;

    out = it->second.ping;
    return true;
}

static bool ManualReadOverlayCorner(char* outCorner, int outSize)
{
    if (!outCorner || outSize <= 0)
        return false;

    outCorner[0] = 0;

    HANDLE h = CreateFileA(g_iniPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD fileSize = GetFileSize(h, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > 65536)
    {
        CloseHandle(h);
        return false;
    }

    char* buffer = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fileSize + 1);
    if (!buffer)
    {
        CloseHandle(h);
        return false;
    }

    DWORD read = 0;
    BOOL ok = ReadFile(h, buffer, fileSize, &read, NULL);
    CloseHandle(h);

    if (!ok || read == 0)
    {
        HeapFree(GetProcessHeap(), 0, buffer);
        return false;
    }

    buffer[read] = 0;
    char* text = buffer;

    if (read >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;

    bool inOverlay = false;
    bool found = false;
    char* line = text;

    while (line && *line)
    {
        char* next = strpbrk(line, "\r\n");
        if (next)
        {
            *next = 0;
            next++;
            while (*next == '\r' || *next == '\n')
                next++;
        }

        TrimString(line);

        if (line[0] == '[')
            inOverlay = (lstrcmpiA(line, "[Overlay]") == 0);
        else if (inOverlay && _strnicmp(line, "Corner", 6) == 0)
        {
            char* equals = strchr(line, '=');
            if (equals)
            {
                equals++;
                TrimString(equals);
                SafeLstrcpynA(outCorner, equals, outSize);
                found = true;
            }
        }

        line = next;
    }

    HeapFree(GetProcessHeap(), 0, buffer);
    return found;
}

static void ApplyCornerString(const char* corner)
{
    if (lstrcmpiA(corner, "top_right") == 0 || lstrcmpiA(corner, "topright") == 0 || lstrcmpiA(corner, "top-right") == 0 || lstrcmpiA(corner, "tr") == 0)
        g_corner = CORNER_TOP_RIGHT;
    else if (lstrcmpiA(corner, "bottom_left") == 0 || lstrcmpiA(corner, "bottomleft") == 0 || lstrcmpiA(corner, "bottom-left") == 0 || lstrcmpiA(corner, "bl") == 0)
        g_corner = CORNER_BOTTOM_LEFT;
    else if (lstrcmpiA(corner, "bottom_right") == 0 || lstrcmpiA(corner, "bottomright") == 0 || lstrcmpiA(corner, "bottom-right") == 0 || lstrcmpiA(corner, "br") == 0)
        g_corner = CORNER_BOTTOM_RIGHT;
    else
        g_corner = CORNER_TOP_LEFT;

    SafeLstrcpynA(g_cornerText, CornerToText(), sizeof(g_cornerText));
}

static void LoadConfig(bool logIt)
{
    LoadLoggingSetting();
    logIt = logIt && g_loggingEnabled;

    char corner[64];
    ZeroMemory(corner, sizeof(corner));

    bool manualCornerFound = ManualReadOverlayCorner(corner, sizeof(corner));
    if (!manualCornerFound)
    {
        GetPrivateProfileStringA("Overlay", "Corner", "top_left", corner, sizeof(corner), g_iniPath);
        TrimString(corner);
    }

    ApplyCornerString(corner);

    g_paddingX = GetPrivateProfileIntA("Overlay", "PaddingX", 18, g_iniPath);
    g_paddingY = GetPrivateProfileIntA("Overlay", "PaddingY", 18, g_iniPath);
    g_refreshMs = GetPrivateProfileIntA("Overlay", "RefreshMs", 1000, g_iniPath);
    if (g_refreshMs < 250)
        g_refreshMs = 250;

    // [Render] Backend = gdi (legacy layered window) | d3d (in-frame Direct3D9 overlay)
    {
        char backend[32];
        ZeroMemory(backend, sizeof(backend));
        GetPrivateProfileStringA("Render", "Backend", "gdi", backend, sizeof(backend), g_iniPath);
        TrimString(backend);
        g_renderBackend = (lstrcmpiA(backend, "d3d") == 0) ? 1 : 0;

        g_d3dSubmitMs = GetPrivateProfileIntA("Render", "SubmitMs", 66, g_iniPath);
        if (g_d3dSubmitMs < 16)  g_d3dSubmitMs = 16;
        if (g_d3dSubmitMs > 500) g_d3dSubmitMs = 500;

        // GDI-window repaint interval. 66ms (~15Hz) is plenty for ping/HP text and
        // cuts DWM recomposition ~4x vs the old 16ms (60Hz) timer.
        g_repaintMs = GetPrivateProfileIntA("Render", "RepaintMs", 66, g_iniPath);
        if (g_repaintMs < 16)   g_repaintMs = 16;
        if (g_repaintMs > 1000) g_repaintMs = 1000;

        // DrawAt = present (after DSFix post; robust) | endscene (fallback)
        char drawAt[32];
        ZeroMemory(drawAt, sizeof(drawAt));
        GetPrivateProfileStringA("Render", "DrawAt", "present", drawAt, sizeof(drawAt), g_iniPath);
        TrimString(drawAt);
        D3DOverlay_SetDrawAtPresent(lstrcmpiA(drawAt, "endscene") != 0);

        D3DOverlay_SetEnabled(g_renderBackend == 1);
    }

    g_exitWithGame = GetPrivateProfileIntA("Settings", "ExitWithGame", 1, g_iniPath) != 0;

    GetPrivateProfileStringA("Overlay", "FontFace", "Tahoma", g_fontFace, sizeof(g_fontFace), g_iniPath);
    TrimString(g_fontFace);
    if (g_fontFace[0] == 0)
        lstrcpyA(g_fontFace, "Tahoma");

    g_fontHeight = GetPrivateProfileIntA("Overlay", "FontHeight", 16, g_iniPath);
    if (g_fontHeight < 10)
        g_fontHeight = 10;
    if (g_fontHeight > 40)
        g_fontHeight = 40;

    // HP can be drawn larger than the rest of the row. Default is exactly 2x the normal row font.
    g_hpFontHeight = GetPrivateProfileIntA("Overlay", "HpFontHeight", g_fontHeight * 2, g_iniPath);
    if (g_hpFontHeight < g_fontHeight)
        g_hpFontHeight = g_fontHeight;
    if (g_hpFontHeight > 80)
        g_hpFontHeight = 80;

    g_lineHeight = GetPrivateProfileIntA("Overlay", "LineHeight", g_hpFontHeight + 4, g_iniPath);
    if (g_lineHeight < g_hpFontHeight + 2)
        g_lineHeight = g_hpFontHeight + 2;
    if (g_lineHeight > 96)
        g_lineHeight = 96;

    // Tighten/loosen the horizontal space between HP, ping marker, and name.
    // Lower = more compact. v79 used 44, which made both sides of the gutter too wide.
    g_markerGutterExtra = GetPrivateProfileIntA("Overlay", "MarkerGutterExtra", 24, g_iniPath);
    if (g_markerGutterExtra < 8)
        g_markerGutterExtra = 8;
    if (g_markerGutterExtra > 80)
        g_markerGutterExtra = 80;

    g_hideLocal = GetPrivateProfileIntA("Overlay", "HideLocal", 0, g_iniPath) != 0;
    g_showHeader = GetPrivateProfileIntA("Overlay", "ShowHeader", 1, g_iniPath) != 0;
    g_forceTopmost = GetPrivateProfileIntA("Overlay", "ForceTopmost", 0, g_iniPath) != 0;
    g_showUnknownWorldRows = GetPrivateProfileIntA("Overlay", "ShowUnknownWorldRows", 1, g_iniPath) != 0;

    // Per-element display toggles. These only control what the overlay draws; the
    // underlying ping/HP subsystems keep running so toggling is instant and reversible.
    g_showPing = GetPrivateProfileIntA("Overlay", "ShowPing", 1, g_iniPath) != 0;
    g_showName = GetPrivateProfileIntA("Overlay", "ShowName", 1, g_iniPath) != 0;
    g_showHpField = GetPrivateProfileIntA("Overlay", "ShowHp", 1, g_iniPath) != 0;
    g_showLocalMarker = GetPrivateProfileIntA("Overlay", "ShowLocalMarker", 1, g_iniPath) != 0;
    g_showDisconnected = GetPrivateProfileIntA("Overlay", "ShowDisconnected", 1, g_iniPath) != 0;

    // Master overlay toggle hotkey. Accepts decimal or hex (e.g. 119 or 0x77) virtual-key codes.
    {
        char toggleKey[32];
        ZeroMemory(toggleKey, sizeof(toggleKey));
        GetPrivateProfileStringA("Overlay", "ToggleKey", "0x72", toggleKey, sizeof(toggleKey), g_iniPath);
        TrimString(toggleKey);
        int vk = (int)strtol(toggleKey, NULL, 0);
        if (vk <= 0 || vk > 255)
            vk = VK_F3;
        g_toggleKey = vk;

        // Optional modifier (Shift/Ctrl/Alt) that must be held with the key. 0 = none.
        char toggleMod[32];
        ZeroMemory(toggleMod, sizeof(toggleMod));
        GetPrivateProfileStringA("Overlay", "ToggleModifier", "0x10", toggleMod, sizeof(toggleMod), g_iniPath);
        TrimString(toggleMod);
        int mod = (int)strtol(toggleMod, NULL, 0);
        if (mod < 0 || mod > 255)
            mod = 0;
        g_toggleModifier = mod;
    }

    // HP is read from the live WORLD actor rows and updated by a separate 60 Hz thread.
    // Defaults are 0x3E8/0x3EC. Override these in decimal if testing shows a different offset.
    g_showHp = GetPrivateProfileIntA("HP", "Enabled", 1, g_iniPath) != 0;
    g_hpPollMs = GetPrivateProfileIntA("HP", "PollMs", 1, g_iniPath);
    if (g_hpPollMs < 1)
        g_hpPollMs = 1;
    if (g_hpPollMs > 100)
        g_hpPollMs = 100;
    g_hpCurrentOffset = GetPrivateProfileIntA("HP", "CurrentOffset", DEFAULT_HP_CURRENT_OFFSET, g_iniPath);
    g_hpMaxOffset = GetPrivateProfileIntA("HP", "MaxOffset", DEFAULT_HP_MAX_OFFSET, g_iniPath);
    g_hpAutoScan = GetPrivateProfileIntA("HP", "AutoScan", 0, g_iniPath) != 0;
    g_hpOneLingerMs = (DWORD)GetPrivateProfileIntA("HP", "OneHpLingerMs", 500, g_iniPath);
    if (g_hpOneLingerMs > 5000)
        g_hpOneLingerMs = 5000;

    // If your arena script instantly refills at 1 HP, a memory poll can miss the literal 1-frame value.
    // This catches the rebound pattern: old sampled HP was lower, new sampled HP jumps upward by a lot.
    g_hpDetectInstantRefill = GetPrivateProfileIntA("HP", "DetectInstantRefill", 1, g_iniPath) != 0;
    g_hpInstantRefillMinGain = GetPrivateProfileIntA("HP", "InstantRefillMinGain", 100, g_iniPath);
    if (g_hpInstantRefillMinGain < 1)
        g_hpInstantRefillMinGain = 1;
    if (g_hpInstantRefillMinGain > 5000)
        g_hpInstantRefillMinGain = 5000;

    g_dumpOverlayData = GetPrivateProfileIntA("Debug", "DumpOverlayData", 0, g_iniPath) != 0;
    g_debugP2PBridge = GetPrivateProfileIntA("Debug", "DebugP2PBridge", 0, g_iniPath) != 0;
    g_identityTtlMs = (DWORD)GetPrivateProfileIntA("Debug", "IdentityTtlMs", 120000, g_iniPath);
    if (g_identityTtlMs < 5000)
        g_identityTtlMs = 5000;

    g_truePingEnabled = GetPrivateProfileIntA("TruePing", "Enabled", 1, g_iniPath) != 0;
    g_truePingDebug = GetPrivateProfileIntA("TruePing", "Debug", 0, g_iniPath) != 0;
    g_truePingVerbose = GetPrivateProfileIntA("TruePing", "Verbose", 0, g_iniPath) != 0;
    g_truePingPreferOverlay = GetPrivateProfileIntA("TruePing", "PreferOverlay", 1, g_iniPath) != 0;
    g_truePingShowSourceMarker = GetPrivateProfileIntA("TruePing", "ShowSourceMarker", 1, g_iniPath) != 0;
    g_truePingSendEnabled = GetPrivateProfileIntA("TruePing", "SendEnabled", 1, g_iniPath) != 0;
    g_truePingReceiveEnabled = GetPrivateProfileIntA("TruePing", "ReceiveEnabled", 1, g_iniPath) != 0;
    g_truePingChannel = GetPrivateProfileIntA("TruePing", "Channel", 63, g_iniPath);
    if (g_truePingChannel < 0) g_truePingChannel = 0;
    if (g_truePingChannel > 255) g_truePingChannel = 255;
    g_truePingAllowGameChannel = GetPrivateProfileIntA("TruePing", "AllowGameChannel", 0, g_iniPath) != 0;
    // v60 join-safety: channel 0/1 are likely used by the game/PTDE or Steam session layer.
    // Reading them from our worker thread can steal packets before Dark Souls sees them, which breaks hosting/joining.
    if (g_truePingChannel < 16 && !g_truePingAllowGameChannel)
    {
        int requestedChannel = g_truePingChannel;
        g_truePingChannel = 63;
        if (logIt)
            WriteLogf("TRUEPING channel guard: requested channel %d is reserved/game-facing; using side-channel %d. Set AllowGameChannel=1 only for diagnostics.", requestedChannel, g_truePingChannel);
    }
    g_truePingSendMs = GetPrivateProfileIntA("TruePing", "SendMs", 1000, g_iniPath);
    if (g_truePingSendMs < 100) g_truePingSendMs = 100;
    if (g_truePingSendMs > 10000) g_truePingSendMs = 10000;
    g_truePingHelloMs = GetPrivateProfileIntA("TruePing", "HelloMs", 2000, g_iniPath);
    if (g_truePingHelloMs < 500) g_truePingHelloMs = 500;
    if (g_truePingHelloMs > 30000) g_truePingHelloMs = 30000;
    g_truePingStaleMs = GetPrivateProfileIntA("TruePing", "StaleMs", 4000, g_iniPath);
    if (g_truePingStaleMs < 1000) g_truePingStaleMs = 1000;
    if (g_truePingStaleMs > 60000) g_truePingStaleMs = 60000;
    g_truePingSendType = GetPrivateProfileIntA("TruePing", "SendType", 2, g_iniPath);
    if (g_truePingSendType < 0) g_truePingSendType = 0;
    if (g_truePingSendType > 3) g_truePingSendType = 3;
    g_truePingSmoothDisplay = GetPrivateProfileIntA("TruePing", "SmoothDisplay", 1, g_iniPath) != 0;
    g_truePingSmoothWeight = GetPrivateProfileIntA("TruePing", "SmoothWeight", 4, g_iniPath);
    if (g_truePingSmoothWeight < 1) g_truePingSmoothWeight = 1;
    if (g_truePingSmoothWeight > 16) g_truePingSmoothWeight = 16;
    g_truePingDisplayMode = GetPrivateProfileIntA("TruePing", "DisplayMode", 2, g_iniPath);
    // Back-compat: SmoothDisplay=0 means raw/latest unless DisplayMode was explicitly set.
    if (!g_truePingSmoothDisplay && GetPrivateProfileIntA("TruePing", "DisplayMode", -1, g_iniPath) < 0)
        g_truePingDisplayMode = 0;
    if (g_truePingDisplayMode < 0) g_truePingDisplayMode = 0;
    if (g_truePingDisplayMode > 2) g_truePingDisplayMode = 2;
    g_truePingBestWindow = GetPrivateProfileIntA("TruePing", "BestWindow", 8, g_iniPath);
    if (g_truePingBestWindow < 1) g_truePingBestWindow = 1;
    if (g_truePingBestWindow > 16) g_truePingBestWindow = 16;
    g_truePingPollSleepMs = GetPrivateProfileIntA("TruePing", "PollSleepMs", 1, g_iniPath);
    if (g_truePingPollSleepMs < 0) g_truePingPollSleepMs = 0;
    if (g_truePingPollSleepMs > 50) g_truePingPollSleepMs = 50;
    g_truePingUseHighResTimer = GetPrivateProfileIntA("TruePing", "UseHighResTimer", 1, g_iniPath) != 0;

    g_webSocketEnabled = GetPrivateProfileIntA("WebSocket", "Enabled", 1, g_iniPath) != 0;
    g_webSocketPort = GetPrivateProfileIntA("WebSocket", "Port", 39876, g_iniPath);
    if (g_webSocketPort < 1024 || g_webSocketPort > 65535)
        g_webSocketPort = 39876;

    g_webSocketSendMs = GetPrivateProfileIntA("WebSocket", "SendMs", 33, g_iniPath);
    if (g_webSocketSendMs < 16)
        g_webSocketSendMs = 16;
    if (g_webSocketSendMs > 1000)
        g_webSocketSendMs = 1000;

    if (logIt)
    {
        WriteLogf(
            "Config: corner='%s' parsed=%s padding=(%d,%d) refresh=%d font='%s' fontHeight=%d hpFontHeight=%d lineHeight=%d hideLocal=%d showHeader=%d topmost=%d showUnknown=%d showPing=%d showHp=%d showName=%d showHpField=%d showLocalMarker=%d toggleKey=0x%02X toggleMod=0x%02X hpPollMs=%d hpOff=%d/%d hpAuto=%d oneHpLinger=%lu instantRefill=%d instantGain=%d oldPing=forever dump=%d p2p=%d ttl=%lu truePing=%d sendEnabled=%d recvEnabled=%d ch=%d allowGameCh=%d sendMs=%d helloMs=%d staleMs=%d sendType=%d verbose=%d sourceMarker=%d smooth=%d smoothWeight=%d displayMode=%d bestWindow=%d pollSleepMs=%d highResTimer=%d",
            corner,
            g_cornerText,
            g_paddingX,
            g_paddingY,
            g_refreshMs,
            g_fontFace,
            g_fontHeight,
            g_hpFontHeight,
            g_lineHeight,
            g_hideLocal ? 1 : 0,
            g_showHeader ? 1 : 0,
            g_forceTopmost ? 1 : 0,
            g_showUnknownWorldRows ? 1 : 0,
            g_showPing ? 1 : 0,
            g_showHp ? 1 : 0,
            g_showName ? 1 : 0,
            g_showHpField ? 1 : 0,
            g_showLocalMarker ? 1 : 0,
            g_toggleKey,
            g_toggleModifier,
            g_hpPollMs,
            g_hpCurrentOffset,
            g_hpMaxOffset,
            g_hpAutoScan ? 1 : 0,
            g_hpOneLingerMs,
            g_hpDetectInstantRefill ? 1 : 0,
            g_hpInstantRefillMinGain,
            g_dumpOverlayData ? 1 : 0,
            g_debugP2PBridge ? 1 : 0,
            g_identityTtlMs,
            g_truePingEnabled ? 1 : 0,
            g_truePingSendEnabled ? 1 : 0,
            g_truePingReceiveEnabled ? 1 : 0,
            g_truePingChannel,
            g_truePingAllowGameChannel ? 1 : 0,
            g_truePingSendMs,
            g_truePingHelloMs,
            g_truePingStaleMs,
            g_truePingSendType,
            g_truePingVerbose ? 1 : 0,
            g_truePingShowSourceMarker ? 1 : 0,
            g_truePingSmoothDisplay ? 1 : 0,
            g_truePingSmoothWeight,
            g_truePingDisplayMode,
            g_truePingBestWindow,
            g_truePingPollSleepMs,
            g_truePingUseHighResTimer ? 1 : 0
        );
    }
}

static HWND FindGameWindow()
{
    DWORD currentPid = GetCurrentProcessId();

    struct EnumData
    {
        DWORD pid;
        HWND best;
        int bestArea;
    };

    EnumData data;
    data.pid = currentPid;
    data.best = NULL;
    data.bestArea = 0;

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL
        {
            EnumData* data = (EnumData*)lParam;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);

            if (pid != data->pid)
                return TRUE;

            if (!IsWindowVisible(hwnd))
                return TRUE;

            LONG style = GetWindowLongA(hwnd, GWL_STYLE);
            LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);

            if ((style & WS_CHILD) != 0)
                return TRUE;

            if ((exStyle & WS_EX_TOOLWINDOW) != 0)
                return TRUE;

            RECT rc;
            if (!GetWindowRect(hwnd, &rc))
                return TRUE;

            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w < 300 || h < 200)
                return TRUE;

            int area = w * h;
            if (area > data->bestArea)
            {
                data->best = hwnd;
                data->bestArea = area;
            }

            return TRUE;
        },
        (LPARAM)&data
    );

    return data.best;
}

static uintptr_t FindPatternInExe(const unsigned char* pattern, const char* mask, size_t patternLen)
{
    uintptr_t base = ExeBase();
    if (base == 0)
        return 0;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (size == 0 || size > 0x04000000)
        return 0;

    for (DWORD i = 0; i + patternLen <= size; i++)
    {
        bool matched = true;
        for (size_t j = 0; j < patternLen; j++)
        {
            if (mask[j] == 'x' && *(unsigned char*)(base + i + j) != pattern[j])
            {
                matched = false;
                break;
            }
        }

        if (matched)
            return base + i;
    }

    return 0;
}

static uintptr_t FindWorldChrBaseAddress()
{
    if (g_worldChrBaseAddress != 0)
        return g_worldChrBaseAddress;

    if (g_triedWorldChrBaseScan)
        return 0;

    g_triedWorldChrBaseScan = true;

    static const unsigned char pattern[] =
    {
        0x00, 0x00, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x8B,
        0x00, 0x00, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF3,
        0x00, 0x00, 0x00, 0xF3, 0x00, 0x00, 0x00, 0x00,
        0x8D, 0x00, 0x00, 0x00, 0x8B
    };

    static const char* mask = "????x??x???x?????x?????x???x????x???x";

    uintptr_t match = FindPatternInExe(pattern, mask, sizeof(pattern));
    if (match == 0)
    {
        WriteLogLine("WorldChrBase AOB scan failed.");
        return 0;
    }

    uintptr_t worldChrBaseAddress = 0;
    if (!SafeRead(match, worldChrBaseAddress))
    {
        WriteLogf("WorldChrBase AOB read failed at %08X", (DWORD)match);
        return 0;
    }

    g_worldChrBaseAddress = worldChrBaseAddress;
    WriteLogf("WorldChrBase AOB match=%08X WorldChrBaseAddress=%08X", (DWORD)match, (DWORD)g_worldChrBaseAddress);
    return g_worldChrBaseAddress;
}

static bool IsReadableHpPair(int hp, int hpMax)
{
    // Do not guess PvP ranges here. The table gives the fields.
    // Only reject impossible read garbage, not legit low-level/low-HP states.
    if (hp < 0 || hpMax < 0)
        return false;

    // HP can be 0 during death/transition. Current HP should not exceed max HP in normal display.
    if (hp > hpMax)
        return false;

    return true;
}

static bool TryReadHpPair(uintptr_t chr, int currentOffset, int maxOffset, int& outHp, int& outHpMax)
{
    if (!LooksLikePointer(chr))
        return false;

    if (currentOffset < 0 || maxOffset < 0)
        return false;

    int hp = -1;
    int hpMax = -1;

    if (!SafeRead(chr + (uintptr_t)currentOffset, hp))
        return false;

    if (!SafeRead(chr + (uintptr_t)maxOffset, hpMax))
        return false;

    if (!IsReadableHpPair(hp, hpMax))
        return false;

    outHp = hp;
    outHpMax = hpMax;
    return true;
}

static bool ReadHpFromChr(uintptr_t chr, int& outHp, int& outHpMax, int& outCurrentOffset, int& outMaxOffset)
{
    outHp = -1;
    outHpMax = -1;
    outCurrentOffset = -1;
    outMaxOffset = -1;

    if (!g_showHp || !LooksLikePointer(chr))
        return false;

    // Table-derived/default pair first. Defaults are 0x2D4 / 0x2D8 for PTDE ChrIns HP.
    if (TryReadHpPair(chr, g_hpCurrentOffset, g_hpMaxOffset, outHp, outHpMax))
    {
        outCurrentOffset = g_hpCurrentOffset;
        outMaxOffset = g_hpMaxOffset;
        return true;
    }

    // Fallback only if explicitly enabled. This is diagnostic, not the normal path.
    if (!g_hpAutoScan)
        return false;

    static const int candidates[][2] =
    {
        { 0x2D4, 0x2D8 },
        { 0x3E8, 0x3EC }
    };

    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++)
    {
        if (TryReadHpPair(chr, candidates[i][0], candidates[i][1], outHp, outHpMax))
        {
            outCurrentOffset = candidates[i][0];
            outMaxOffset = candidates[i][1];
            return true;
        }
    }

    return false;
}

static void AddWorldActorRow(std::vector<WorldActorRow>& rows, const char* name, int slot, uintptr_t chr)
{
    WorldActorRow row;

    row.slotName = name;
    row.slotIndex = slot;
    row.valid = false;
    row.chr = chr;
    row.playerParam = 0;
    row.phantomData = 0;
    row.playerNo = -1;
    row.isPhantom = -1;
    row.phantomType = -1;
    row.redKind = -1;
    row.invadeType = -999;
    row.vowType = -1;
    row.chrType = -1;
    row.teamType = -1;
    row.playerParamChrType = -1;
    row.hasHp = false;
    row.hp = -1;
    row.hpMax = -1;
    row.hpCurrentOffset = -1;
    row.hpMaxOffset = -1;
    row.inGameName.clear();

    row.valid = LooksLikePointer(chr);

    if (row.valid)
    {
        SafeRead(chr + OFF_CHR_CHR_TYPE, row.chrType);
        SafeRead(chr + OFF_CHR_TEAM_TYPE, row.teamType);
        SafeRead(chr + OFF_CHR_PLAYER_PARAM, row.playerParam);
        SafeRead(chr + OFF_CHR_PHANTOM_DATA, row.phantomData);

        if (LooksLikePointer(row.playerParam))
        {
            SafeRead(row.playerParam + OFF_PLAYERPARAM_PLAYERNO, row.playerNo);
            SafeRead(row.playerParam + OFF_PLAYERPARAM_CHR_TYPE, row.playerParamChrType);
            SafeRead(row.playerParam + OFF_PLAYERPARAM_INVADE_TYPE, row.invadeType);
            unsigned char vowType = 0xFF;
            if (SafeRead(row.playerParam + OFF_PLAYERPARAM_VOW_TYPE, vowType))
                row.vowType = (int)vowType;
            row.inGameName = SafeReadWideString(row.playerParam + OFF_PLAYERPARAM_NAME_WIDE, 32);
        }

        if (LooksLikePointer(row.phantomData))
        {
            unsigned char isPhantom = 0;
            unsigned char phantomType = 0;
            unsigned char redKind = 0xFF;
            SafeRead(row.phantomData + OFF_PHANTOM_IS_PHANTOM, isPhantom);
            SafeRead(row.phantomData + OFF_PHANTOM_TYPE, phantomType);
            SafeRead(row.phantomData + OFF_PHANTOM_RED_KIND, redKind);
            row.isPhantom = (int)isPhantom;
            row.phantomType = (int)phantomType;
            row.redKind = (int)redKind;
        }

        int hp = -1;
        int hpMax = -1;
        int hpCurrentOffset = -1;
        int hpMaxOffset = -1;
        if (ReadHpFromChr(row.chr, hp, hpMax, hpCurrentOffset, hpMaxOffset))
        {
            row.hasHp = true;
            row.hp = hp;
            row.hpMax = hpMax;
            row.hpCurrentOffset = hpCurrentOffset;
            row.hpMaxOffset = hpMaxOffset;
        }
    }

    rows.push_back(row);
}

static std::vector<WorldActorRow> ReadWorldActors()
{
    std::vector<WorldActorRow> rows;

    uintptr_t worldChrBaseAddress = FindWorldChrBaseAddress();
    if (!LooksLikePointer(worldChrBaseAddress))
        return rows;

    uintptr_t worldChrBase = 0;
    uintptr_t selfChr = 0;
    uintptr_t mpRoot = 0;
    uintptr_t mpChr1 = 0;
    uintptr_t mpChr2 = 0;
    uintptr_t mpChr3 = 0;

    SafeRead(worldChrBaseAddress, worldChrBase);
    if (!LooksLikePointer(worldChrBase))
        return rows;

    SafeRead(worldChrBase + OFF_WORLDCHR_SELF, selfChr);
    AddWorldActorRow(rows, "SELF", 0, selfChr);

    if (LooksLikePointer(selfChr))
    {
        SafeRead(selfChr + OFF_CHR_MP_ROOT, mpRoot);
        if (LooksLikePointer(mpRoot))
        {
            SafeRead(mpRoot + OFF_MPCHR1, mpChr1);
            SafeRead(mpRoot + OFF_MPCHR2, mpChr2);
            SafeRead(mpRoot + OFF_MPCHR3, mpChr3);
        }
    }

    AddWorldActorRow(rows, "R1", 1, mpChr1);
    AddWorldActorRow(rows, "R2", 2, mpChr2);
    AddWorldActorRow(rows, "R3", 3, mpChr3);
    return rows;
}

static bool IsValidPlayerNo(int playerNo)
{
    // PTDE can reuse/advance player numbers after a phantom leaves/rejoins.
    // The log showed a live rejoin row with pno=5, so 1..4 is too strict.
    return playerNo >= 0 && playerNo < MAX_TRACKED_PLAYER_NO;
}

static bool WorldHasLivePlayerNo(const std::vector<WorldActorRow>& worldRows, int playerNo)
{
    for (size_t i = 0; i < worldRows.size(); i++)
    {
        if (worldRows[i].valid && worldRows[i].playerNo == playerNo)
            return true;
    }

    return false;
}

static bool AddPingFromConnection(uintptr_t connection, std::unordered_map<uint64_t, PingInfo>& out)
{
    if (!LooksLikePointer(connection))
        return false;

    int status = 0;
    int cid = -1;
    uint64_t steamId = 0;
    int ping = -1;

    SafeRead(connection + OFF_CONNECTION_STATUS, status);
    SafeRead(connection + OFF_CONNECTION_CID, cid);
    SafeRead(connection + OFF_CONNECTION_STEAMID64, steamId);
    SafeRead(connection + OFF_CONNECTION_PING, ping);

    if (status <= 2)
        return false;

    if (steamId == 0)
        return false;

    PingInfo info;
    info.status = status;
    info.cid = cid;
    info.ping = ping;
    info.connection = connection;
    out[steamId] = info;
    return true;
}

static int ReadDSCMP2PConnectionList(uintptr_t connectionList, std::unordered_map<uint64_t, PingInfo>& out)
{
    if (!LooksLikePointer(connectionList))
        return 0;

    uintptr_t entry = 0;
    if (!SafeRead(connectionList, entry))
        return 0;

    if (!LooksLikePointer(entry))
        return 0;

    int added = 0;
    int count = 0;

    while (entry != 0 && entry != connectionList && count < 128)
    {
        uintptr_t connection = 0;
        SafeRead(entry + OFF_LISTNODE_CONNECTION, connection);

        if (AddPingFromConnection(connection, out))
            added++;

        uintptr_t next = 0;
        if (!SafeRead(entry, next))
            break;

        if (next == entry)
            break;

        entry = next;
        count++;
    }

    return added;
}

static std::unordered_map<uint64_t, PingInfo> ReadPingsBySteamId()
{
    std::unordered_map<uint64_t, PingInfo> result;

    uintptr_t sessionManagerAddress = Rva(OFF_SESSION_MANAGER);
    uintptr_t p2pSystem = 0;
    SafeRead(sessionManagerAddress + OFF_SESSION_P2P_SYSTEM, p2pSystem);

    if (LooksLikePointer(p2pSystem))
    {
        uintptr_t connectionList = 0;
        SafeRead(p2pSystem + OFF_P2P_CONNECTION_LIST, connectionList);
        if (LooksLikePointer(connectionList))
            ReadDSCMP2PConnectionList(connectionList, result);
    }

    if (result.empty())
    {
        uintptr_t sessionManagerPtr = 0;
        SafeRead(Rva(OFF_SESSION_MANAGER), sessionManagerPtr);

        if (LooksLikePointer(sessionManagerPtr))
        {
            uintptr_t fallbackP2PSystem = 0;
            SafeRead(sessionManagerPtr + OFF_SESSION_P2P_SYSTEM, fallbackP2PSystem);

            if (LooksLikePointer(fallbackP2PSystem))
            {
                uintptr_t fallbackConnectionList = 0;
                SafeRead(fallbackP2PSystem + OFF_P2P_CONNECTION_LIST, fallbackConnectionList);
                if (LooksLikePointer(fallbackConnectionList))
                    ReadDSCMP2PConnectionList(fallbackConnectionList, result);
            }
        }
    }

    return result;
}

static std::unordered_map<uint64_t, SteamNodeInfo> ReadSteamNodesById()
{
    std::unordered_map<uint64_t, SteamNodeInfo> result;
    std::unordered_map<uint64_t, PingInfo> pingMap = ReadPingsBySteamId();

    uintptr_t nodeList = 0;
    int nodeCount = 0;

    SafeRead(Rva(OFF_STEAM_NODE_LIST), nodeList);
    SafeRead(Rva(OFF_STEAM_NODE_COUNT), nodeCount);

    if (!LooksLikePointer(nodeList))
        return result;

    if (nodeCount < 0)
        nodeCount = 0;

    if (nodeCount > 32)
        nodeCount = 32;

    for (int i = 0; i < nodeCount; i++)
    {
        uintptr_t node = 0;
        if (!SafeRead(nodeList + i * 4, node))
            continue;

        if (!LooksLikePointer(node))
            continue;

        uintptr_t steamPlayerData = 0;
        uintptr_t onlineIdData = 0;

        SafeRead(node + OFF_NODE_STEAM_PLAYER_DATA, steamPlayerData);
        if (!LooksLikePointer(steamPlayerData))
            continue;

        SafeRead(steamPlayerData + OFF_STEAM_PLAYER_ONLINE_ID_DATA, onlineIdData);

        std::string name = SafeReadAutoString(steamPlayerData + OFF_STEAM_PLAYER_NAME, 32);
        std::string steamIdText;
        if (LooksLikePointer(onlineIdData))
            steamIdText = SafeReadAutoString(onlineIdData + OFF_STEAM_ONLINE_ID_STRING, 32);

        uint64_t parsedSteamId = ParseSteamIdTextHex(steamIdText);
        if (parsedSteamId == 0)
            continue;

        SteamNodeInfo info;
        ZeroMemory(&info.ping, sizeof(info.ping));
        info.name = name.empty() ? "(unknown)" : name;
        info.steamIdText = steamIdText;
        info.steamId = parsedSteamId;
        info.isLocal = (i == 0);
        if (info.isLocal && parsedSteamId != 0)
            g_localSteamId = parsedSteamId;
        info.hasPing = false;

        std::unordered_map<uint64_t, PingInfo>::iterator it = pingMap.find(parsedSteamId);
        if (it != pingMap.end())
        {
            info.hasPing = true;
            info.ping = it->second;
            RememberCachedPing(parsedSteamId, info.name, info.ping);
        }
        else
        {
            PingInfo cachedPing;
            ZeroMemory(&cachedPing, sizeof(cachedPing));
            if (FindCachedPingBySteamId(parsedSteamId, cachedPing) || FindCachedPingByName(info.name, cachedPing))
            {
                info.hasPing = true;
                info.ping = cachedPing;
            }
        }

        if (parsedSteamId != 0 && !info.name.empty() && info.name != "(unknown)")
            g_nameCacheBySteamId[parsedSteamId] = info.name;

        result[parsedSteamId] = info;
    }

    return result;
}

static bool SnapshotIdentityForPlayerNo(int playerNo, PlayerIdentity& out)
{
    ZeroMemory(&out, sizeof(out));

    if (!IsValidPlayerNo(playerNo))
        return false;

    if (!g_identityLockReady)
        return false;

    EnterCriticalSection(&g_identityLock);
    out = g_identityByPlayerNo[playerNo];
    LeaveCriticalSection(&g_identityLock);

    if (out.steamId == 0)
        return false;

    ULONGLONG now = GetTickCount64();
    if (out.lastSeenTick != 0 && now - out.lastSeenTick > g_identityTtlMs)
        return false;

    return true;
}

static void PruneIdentitiesForWorldRows(const std::vector<WorldActorRow>& worldRows)
{
    if (!g_identityLockReady)
        return;

    ULONGLONG now = GetTickCount64();

    EnterCriticalSection(&g_identityLock);

    for (std::unordered_map<uint64_t, ULONGLONG>::iterator it = g_recentP2PRemoteSteamIds.begin(); it != g_recentP2PRemoteSteamIds.end(); )
    {
        uint64_t steamId = it->first;
        ULONGLONG seenTick = it->second;
        bool isLocal = (g_localSteamId != 0 && steamId == g_localSteamId);
        bool expired = (seenTick != 0 && now - seenTick > RECENT_P2P_REMOTE_TTL_MS);

        if (steamId == 0 || isLocal || expired)
            it = g_recentP2PRemoteSteamIds.erase(it);
        else
            ++it;
    }

    for (int pno = 0; pno < MAX_TRACKED_PLAYER_NO; pno++)
    {
        PlayerIdentity& id = g_identityByPlayerNo[pno];
        if (id.steamId == 0)
            continue;

        bool alive = WorldHasLivePlayerNo(worldRows, pno);
        ULONGLONG age = (id.lastSeenTick == 0) ? 0 : (now - id.lastSeenTick);
        bool expired = (id.lastSeenTick != 0 && age > g_identityTtlMs);
        bool absentTooLong = (!alive && id.lastSeenTick != 0 && age > 15000);

        // Do not use cached IDs to display rows; BuildOverlayRows only renders live WORLD rows.
        // But keep a short grace window because the packet can arrive before the body/slot appears.
        if (expired || absentTooLong)
        {
            WriteLogf(
                "P2P_ID_PRUNE pno=%d steam=%08X%08X alive=%d expired=%d absentTooLong=%d age=%lu",
                pno,
                (DWORD)(id.steamId >> 32),
                (DWORD)(id.steamId & 0xFFFFFFFF),
                alive ? 1 : 0,
                expired ? 1 : 0,
                absentTooLong ? 1 : 0,
                (DWORD)age
            );
            ZeroMemory(&id, sizeof(id));
        }
    }

    LeaveCriticalSection(&g_identityLock);
}

// InvadeType is read from Ashley PlayerParam +0x110. These values appear to match
// the global_event.lua INVADE_TYPE registration order:
//   0  = None / host
//   1  = NormalWhite        -> white summon
//   2  = NormalBlack        -> red-sign / red summon route
//   3  = ForceJoinBlack     -> red-eye / hostile red invader route
//   4  = DetectBlack        -> red-eye / hostile red invader route
//   7  = Nito               -> Gravelord Servant / Spirit of Vengeance route
//   8  = ThievesGuild       -> Forest Hunter / Cat Ring route
//   9  = OtoutoUmbasa       -> Darkmoon / blue-eye Spirit of Vengeance route
//   10 = Dragonewt          -> Path of the Dragon summon route
//
// Important: these color roles are entry-route roles, not covenant colors.
// Vow_Type is only used to split Warrior of Sunlight from normal white summons.
static bool IsInvadeTypeValue(int invadeType, int value)
{
    return invadeType == value || invadeType == -value;
}

static bool IsGravelordBlueInvadeType(int invadeType)
{
    return IsInvadeTypeValue(invadeType, 7);
}

static bool IsDarkmoonBlueInvadeType(int invadeType)
{
    return IsInvadeTypeValue(invadeType, 9);
}

static bool IsDragonSummonInvadeType(int invadeType)
{
    return IsInvadeTypeValue(invadeType, 10);
}

static bool IsNormalWhiteSummonInvadeType(int invadeType)
{
    return IsInvadeTypeValue(invadeType, 1);
}

static bool IsNormalBlackSummonInvadeType(int invadeType)
{
    return IsInvadeTypeValue(invadeType, 2);
}

static bool IsForestHunterInvadeType(int invadeType)
{
    return IsInvadeTypeValue(invadeType, 8);
}

static bool IsRedHostileInvadeType(int invadeType)
{
    // NormalBlack (2) is the red-sign / red summon route. Hostile red-eye style
    // invasions are ForceJoinBlack / DetectBlack. Do not color red signs as
    // black-text red-border invaders.
    return IsInvadeTypeValue(invadeType, 3) ||
        IsInvadeTypeValue(invadeType, 4);
}

// Ashley Vow_Type value used only for the white-summon sunlight split:
//   3 = Warrior of Sunlight
static bool IsSunlightVowType(int vowType)
{
    return vowType == 3;
}

static int PhantomTypeBase(int phantomType)
{
    if (phantomType < 0)
        return phantomType;

    // The Ashley/phantom byte can carry high-bit/state flags. Example from the
    // uploaded log: phType=18 (0x12), which is still the blue bucket (low nibble 2).
    // Treat the low nibble as the visible phantom color bucket so flagged blue/red
    // rows don't fall through to gray "Phantom" styling.
    int low = phantomType & 0x0F;
    if (low >= 0 && low <= 3)
        return low;

    return phantomType;
}

static bool IsSunlightSummonRoute(int isPhantom, int phantomType, int invadeType, int vowType)
{
    UNREFERENCED_PARAMETER(invadeType);

    // Sunlight is the one covenant-based split the overlay is supposed to make,
    // but only inside the white-summon route. v70 showed that normal white signs
    // can expose phType=2, so InvadeType=NormalWhite must be accepted here.
    return isPhantom != 0 &&
        IsSunlightVowType(vowType) &&
        (IsNormalWhiteSummonInvadeType(invadeType) || PhantomTypeBase(phantomType) == 1);
}

static bool IsNormalWhiteSummonRoute(int isPhantom, int invadeType)
{
    // Uploaded v70 log proves a normal white-sign summon can expose phType=2
    // while PlayerParam InvadeType is correctly 1. Treat InvadeType=NormalWhite
    // as the authoritative route, otherwise white signs get miscolored as blue.
    return isPhantom != 0 && IsNormalWhiteSummonInvadeType(invadeType);
}

static bool IsBlueAppearanceRoute(int isPhantom, int phantomType, int invadeType)
{
    // phType=2 is not enough. Normal white signs can also show phType=2, so
    // never classify the blue appearance bucket as blue-route when InvadeType
    // already says NormalWhite.
    return isPhantom != 0 &&
        !IsNormalWhiteSummonInvadeType(invadeType) &&
        PhantomTypeBase(phantomType) == 2;
}

static bool IsGravelordRoute(int isPhantom, int invadeType);
static bool IsDarkmoonRoute(int isPhantom, int invadeType);

static bool IsGravelordRouteFull(int isPhantom, int phantomType, int invadeType, int vowType)
{
    // Primary: entry route. Fallback: blue phantom appearance + Gravelord vow.
    // The fallback is bounded by the blue appearance bucket, so this is not a
    // generic covenant recolor. It only disambiguates blue-route phantoms when
    // InvadeType is missing/stale.
    return IsGravelordRoute(isPhantom, invadeType) ||
        (IsBlueAppearanceRoute(isPhantom, phantomType, invadeType) && vowType == 6);
}

static bool IsDarkmoonRouteFull(int isPhantom, int phantomType, int invadeType, int vowType)
{
    // Primary: entry route. Fallback: blue phantom appearance + Darkmoon vow.
    // If neither signal is present, the generic blue appearance fallback below
    // still uses black text with a blue border rather than blue text/black border.
    return IsDarkmoonRoute(isPhantom, invadeType) ||
        (IsBlueAppearanceRoute(isPhantom, phantomType, invadeType) && vowType == 8);
}

static bool IsDragonSummonRoute(int isPhantom, int invadeType)
{
    return isPhantom != 0 && IsDragonSummonInvadeType(invadeType);
}

static bool IsForestHunterRoute(int isPhantom, int invadeType)
{
    return isPhantom != 0 && IsForestHunterInvadeType(invadeType);
}

static bool IsGravelordRoute(int isPhantom, int invadeType)
{
    return isPhantom != 0 && IsGravelordBlueInvadeType(invadeType);
}

static bool IsDarkmoonRoute(int isPhantom, int invadeType)
{
    return isPhantom != 0 && IsDarkmoonBlueInvadeType(invadeType);
}

static bool IsRedSummonRoute(int isPhantom, int phantomType, int redKind, int invadeType)
{
    if (isPhantom == 0)
        return false;

    // NormalBlack is the normal red-sign / red summon route. This must win over
    // the generic red phantom bucket, otherwise red summons render as invaders.
    if (IsNormalBlackSummonInvadeType(invadeType))
        return true;

    // Older diagnostic builds saw phantomData+0x96 == 3 for red-sign summons.
    return PhantomTypeBase(phantomType) == 3 && redKind == 3;
}

static bool IsRedInvaderRoute(int isPhantom, int phantomType, int redKind, int invadeType)
{
    if (isPhantom == 0)
        return false;

    // Do not let special entry routes collapse back into the generic red invader bucket.
    if (IsDragonSummonInvadeType(invadeType) ||
        IsForestHunterInvadeType(invadeType) ||
        IsGravelordBlueInvadeType(invadeType) ||
        IsDarkmoonBlueInvadeType(invadeType))
        return false;

    if (IsRedSummonRoute(isPhantom, phantomType, redKind, invadeType))
        return false;

    // ForceJoinBlack / DetectBlack hostile red routes get the black-text red-border style even if the
    // redKind byte is absent or ambiguous.
    if (IsRedHostileInvadeType(invadeType))
        return true;

    // Fallback from older builds: red bucket with redKind 0 is a true invasion.
    return PhantomTypeBase(phantomType) == 3 && redKind == 0;
}


static const int STICKY_ROUTE_NONE = 0;
static const int STICKY_ROUTE_SUNLIGHT = 1;
static const int STICKY_ROUTE_DRAGON = 2;
static const int STICKY_ROUTE_FOREST = 3;
static const int STICKY_ROUTE_GRAVELORD = 4;
static const int STICKY_ROUTE_DARKMOON = 5;
static const int STICKY_ROUTE_RED_SUMMON = 6;
static const int STICKY_ROUTE_RED_INVADER = 7;
static const int STICKY_ROUTE_WHITE_SUMMON = 8;
static const int STICKY_ROUTE_BLUE_GENERIC = 9;
static const int STICKY_ROUTE_HOST_LIVE_TYPE = 10;

struct StickyRouteInfo
{
    int kind = STICKY_ROUTE_NONE;
    uint64_t steamId = 0;
    std::string name;
    ULONGLONG lockTick = 0;
    ULONGLONG lastSeenTick = 0;
};

static StickyRouteInfo g_stickyRouteByPlayerNo[MAX_TRACKED_PLAYER_NO];

static std::unordered_map<std::string, StickyRouteInfo> g_stickyRouteByIdentityKey;
static const DWORD COMMITTED_STYLE_DROP_GRACE_MS = 0;

static const char* CommittedStyleKeyPrefixSteam()
{
    return "steam:";
}

static std::string FormatSteamIdKey(uint64_t steamId)
{
    if (steamId == 0)
        return std::string();

    char key[64];
    wsprintfA(key, "%s%I64u", CommittedStyleKeyPrefixSteam(), steamId);
    return std::string(key);
}

static std::string FormatPlayerNoKey(int playerNo)
{
    if (!IsValidPlayerNo(playerNo))
        return std::string();

    char key[32];
    wsprintfA(key, "pno:%d", playerNo);
    return std::string(key);
}

static void AddUniqueStyleKey(std::vector<std::string>& keys, const std::string& key)
{
    if (key.empty())
        return;

    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i] == key)
            return;
    }

    keys.push_back(key);
}

static void CollectCommittedStyleKeys(const OverlayRow& row, std::vector<std::string>& keys)
{
    keys.clear();

    AddUniqueStyleKey(keys, FormatSteamIdKey(row.steamId));

    std::string nameKey = NormalizeNameKey(row.name);
    if (!nameKey.empty())
        AddUniqueStyleKey(keys, std::string("name:") + nameKey);

    std::string worldNameKey = NormalizeNameKey(row.worldName);
    if (!worldNameKey.empty())
        AddUniqueStyleKey(keys, std::string("name:") + worldNameKey);

    // PlayerNo is intentionally the lowest-quality key. It preserves the style
    // through brief Steam/name bridge churn, but identity/name keys prevent the
    // color from being tied only to one unstable WORLD row field.
    AddUniqueStyleKey(keys, FormatPlayerNoKey(row.playerNo));
}

static int CommittedStyleRank(int kind)
{
    switch (kind)
    {
    case STICKY_ROUTE_SUNLIGHT:
    case STICKY_ROUTE_DRAGON:
    case STICKY_ROUTE_FOREST:
    case STICKY_ROUTE_GRAVELORD:
    case STICKY_ROUTE_DARKMOON:
    case STICKY_ROUTE_RED_SUMMON:
    case STICKY_ROUTE_RED_INVADER:
        return 3;
    case STICKY_ROUTE_WHITE_SUMMON:
    case STICKY_ROUTE_BLUE_GENERIC:
        return 2;
    default:
        return 0;
    }
}

static bool PickBetterCommittedStyle(const StickyRouteInfo& candidate, StickyRouteInfo& best)
{
    if (candidate.kind == STICKY_ROUTE_NONE)
        return false;

    if (best.kind == STICKY_ROUTE_NONE ||
        CommittedStyleRank(candidate.kind) > CommittedStyleRank(best.kind) ||
        (CommittedStyleRank(candidate.kind) == CommittedStyleRank(best.kind) && candidate.lockTick < best.lockTick))
    {
        best = candidate;
        return true;
    }

    return false;
}

static StickyRouteInfo FindCommittedStyleForRow(const OverlayRow& row)
{
    StickyRouteInfo best;

    if (IsValidPlayerNo(row.playerNo))
        PickBetterCommittedStyle(g_stickyRouteByPlayerNo[row.playerNo], best);

    std::vector<std::string> keys;
    CollectCommittedStyleKeys(row, keys);

    for (size_t i = 0; i < keys.size(); ++i)
    {
        std::unordered_map<std::string, StickyRouteInfo>::const_iterator it = g_stickyRouteByIdentityKey.find(keys[i]);
        if (it != g_stickyRouteByIdentityKey.end())
            PickBetterCommittedStyle(it->second, best);
    }

    return best;
}

static void StoreCommittedStyleForRow(const OverlayRow& row, const StickyRouteInfo& info)
{
    if (info.kind == STICKY_ROUTE_NONE)
        return;

    if (IsValidPlayerNo(row.playerNo))
        g_stickyRouteByPlayerNo[row.playerNo] = info;

    std::vector<std::string> keys;
    CollectCommittedStyleKeys(row, keys);

    for (size_t i = 0; i < keys.size(); ++i)
        g_stickyRouteByIdentityKey[keys[i]] = info;
}

static bool RowKeepsCommittedStyleAlive(const OverlayRow& row)
{
    // Committed styles are phantom-entry styles. They should not survive through
    // load screens, returns home, or a player becoming a normal host/local row.
    // Host/local non-phantom rows get their own color directly and must not keep
    // a previous phantom style alive by matching the same name/playerNo.
    return row.isPhantom != 0;
}

static void ClearCommittedStyleCache(const char* reason)
{
    bool hadAny = !g_stickyRouteByIdentityKey.empty();

    for (int i = 0; i < MAX_TRACKED_PLAYER_NO; ++i)
    {
        if (g_stickyRouteByPlayerNo[i].kind != STICKY_ROUTE_NONE)
            hadAny = true;
        g_stickyRouteByPlayerNo[i] = StickyRouteInfo();
    }

    g_stickyRouteByIdentityKey.clear();

    if (hadAny)
    {
        char buf[256];
        wsprintfA(buf, "COMMITTED_STYLE_CLEAR reason=%s", reason ? reason : "unknown");
        WriteLogLine(buf);
    }
}

static int DetectCommittedStyleKindRaw(int isPhantom, int phantomType, int redKind, int invadeType, int vowType)
{
    if (isPhantom == 0)
        return STICKY_ROUTE_NONE;

    // Specific/signed routes first. These are entry-route roles, not generic covenant colors.
    if (IsSunlightSummonRoute(isPhantom, phantomType, invadeType, vowType))
        return STICKY_ROUTE_SUNLIGHT;
    if (IsDragonSummonRoute(isPhantom, invadeType))
        return STICKY_ROUTE_DRAGON;
    if (IsForestHunterRoute(isPhantom, invadeType))
        return STICKY_ROUTE_FOREST;
    if (IsGravelordRouteFull(isPhantom, phantomType, invadeType, vowType))
        return STICKY_ROUTE_GRAVELORD;
    if (IsDarkmoonRouteFull(isPhantom, phantomType, invadeType, vowType))
        return STICKY_ROUTE_DARKMOON;
    if (IsRedSummonRoute(isPhantom, phantomType, redKind, invadeType))
        return STICKY_ROUTE_RED_SUMMON;
    if (IsRedInvaderRoute(isPhantom, phantomType, redKind, invadeType))
        return STICKY_ROUTE_RED_INVADER;

    if (IsNormalWhiteSummonRoute(isPhantom, invadeType))
        return STICKY_ROUTE_WHITE_SUMMON;

    // Generic visible buckets are also worth committing so a temporarily stale
    // WORLD/PlayerParam poll cannot flash a live row gray. They remain upgradeable
    // if a more specific subtype is observed later, e.g. White -> Sunlight or
    // GenericBlue -> Gravelord/Darkmoon.
    switch (PhantomTypeBase(phantomType))
    {
    case 1:
        return STICKY_ROUTE_WHITE_SUMMON;
    case 2:
        return STICKY_ROUTE_BLUE_GENERIC;
    default:
        return STICKY_ROUTE_NONE;
    }
}

static bool IsMeaningfulOverlayName(const std::string& name)
{
    return !name.empty() && _stricmp(name.c_str(), "Unknown") != 0 && _stricmp(name.c_str(), "(unknown)") != 0;
}

static void UpdateCommittedStyleIdentity(StickyRouteInfo& info, const OverlayRow& row)
{
    if (info.steamId == 0 && row.steamId != 0)
        info.steamId = row.steamId;

    if (IsMeaningfulOverlayName(row.name))
        info.name = row.name;
}

static bool CommittedStyleHardMismatch(const StickyRouteInfo& info, const OverlayRow& row)
{
    // SteamID is the only hard identity key we trust enough to reset a committed
    // style while the same playerNo is still visible. Names can legitimately change
    // from playerParam character name to Steam persona once the bridge catches up.
    return info.steamId != 0 && row.steamId != 0 && info.steamId != row.steamId;
}

static bool ShouldUpgradeCommittedStyle(int oldKind, int newKind)
{
    if (newKind == STICKY_ROUTE_NONE)
        return false;

    if (oldKind == STICKY_ROUTE_NONE)
        return true;

    // Allow only generic -> more-specific upgrades. Never downgrade a committed
    // specific color because one later poll returned stale/blank route bytes.
    if (oldKind == STICKY_ROUTE_WHITE_SUMMON && newKind == STICKY_ROUTE_SUNLIGHT)
        return true;

    if (oldKind == STICKY_ROUTE_BLUE_GENERIC &&
        (newKind == STICKY_ROUTE_DARKMOON || newKind == STICKY_ROUTE_GRAVELORD))
        return true;

    // v70 bugfix: NormalWhite InvadeType is authoritative. A normal white sign
    // can temporarily look like the blue appearance bucket (phType=2), which used
    // to commit blue/darkmoon styling before the white route was considered.
    if ((oldKind == STICKY_ROUTE_BLUE_GENERIC || oldKind == STICKY_ROUTE_DARKMOON || oldKind == STICKY_ROUTE_GRAVELORD) &&
        (newKind == STICKY_ROUTE_WHITE_SUMMON || newKind == STICKY_ROUTE_SUNLIGHT))
        return true;

    return false;
}

static void ApplyStickyRouteClassification(OverlayRow& row)
{
    row.stickyRoute = STICKY_ROUTE_NONE;
    row.stickyRouteAgeMs = 0;

    // Do not apply phantom-entry committed styles to non-phantom rows.
    // This was the source of colors leaking through loading screens/returns: the
    // same playerNo/name could keep a previous phantom color alive after the
    // phantom had gone home or the local client returned to its own world.
    if (!RowKeepsCommittedStyleAlive(row))
        return;

    ULONGLONG now = GetTickCount64();

    StickyRouteInfo info = FindCommittedStyleForRow(row);

    if (CommittedStyleHardMismatch(info, row))
        info = StickyRouteInfo();

    int current = DetectCommittedStyleKindRaw(row.isPhantom, row.phantomType, row.redKind, row.invadeType, row.vowType);
    if (ShouldUpgradeCommittedStyle(info.kind, current))
    {
        info.kind = current;
        info.lockTick = now;
    }

    if (info.kind == STICKY_ROUTE_NONE)
        return;

    info.lastSeenTick = now;
    UpdateCommittedStyleIdentity(info, row);
    StoreCommittedStyleForRow(row, info);

    row.stickyRoute = info.kind;
    row.stickyRouteAgeMs = (info.lockTick != 0) ? (now - info.lockTick) : 0;

    // Keep old callers/debug JSON sane. This does not claim to discover covenant
    // globally; it only preserves a style we already observed on this live row.
    if (info.kind == STICKY_ROUTE_SUNLIGHT && row.vowType != 3)
        row.vowType = 3;
}

static void PruneCommittedStyleCacheForVisibleRows(const std::vector<OverlayRow>& rows)
{
    bool liveByPlayerNo[MAX_TRACKED_PLAYER_NO] = {};
    std::unordered_set<std::string> liveKeys;
    bool anyLivePhantomStyleRow = false;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (!RowKeepsCommittedStyleAlive(rows[i]))
            continue;

        anyLivePhantomStyleRow = true;

        if (IsValidPlayerNo(rows[i].playerNo))
            liveByPlayerNo[rows[i].playerNo] = true;

        std::vector<std::string> keys;
        CollectCommittedStyleKeys(rows[i], keys);
        for (size_t k = 0; k < keys.size(); ++k)
            liveKeys.insert(keys[k]);
    }

    if (!anyLivePhantomStyleRow)
    {
        ClearCommittedStyleCache("no_live_phantom_rows");
        return;
    }

    // No grace period here. The color lock is supposed to last exactly as long
    // as the phantom/name stays on the roster. A grace period was useful for
    // one-frame hiccups, but it also carried phantom colors across loading
    // screens and returns home. If the row drops, the commitment drops.
    for (int playerNo = 0; playerNo < MAX_TRACKED_PLAYER_NO; ++playerNo)
    {
        if (!liveByPlayerNo[playerNo] && g_stickyRouteByPlayerNo[playerNo].kind != STICKY_ROUTE_NONE)
            g_stickyRouteByPlayerNo[playerNo] = StickyRouteInfo();
    }

    for (std::unordered_map<std::string, StickyRouteInfo>::iterator it = g_stickyRouteByIdentityKey.begin(); it != g_stickyRouteByIdentityKey.end(); )
    {
        if (liveKeys.find(it->first) == liveKeys.end())
        {
            std::unordered_map<std::string, StickyRouteInfo>::iterator dead = it;
            ++it;
            g_stickyRouteByIdentityKey.erase(dead);
        }
        else
            ++it;
    }
}


static bool IsRecognizedLiveChrTeam(int chrType, int teamType)
{
    if (chrType < 0 || teamType < 0)
        return false;

    if ((chrType == 0 && teamType == 1) || (chrType == 8 && teamType == 4))
        return true; // human / hollow host appearance
    if (chrType == 1 && teamType == 2)
        return true; // white / sunlight phantom appearance
    if (chrType == 2 && (teamType == 16 || teamType == 3))
        return true; // black/red phantom appearance. TeamType 16 is DS-Gadget's PTDE value; 3 is table fallback.
    if (chrType == 2 && teamType == 17)
        return true; // blue-route appearance bucket: Forest/Darkmoon share this in DS-Gadget.
    if (chrType == 2 && teamType == 18)
        return true; // Path of the Dragon appearance bucket

    return false;
}

static int StyleKindFromLiveChrTeam(int chrType, int teamType, int invadeType, int vowType)
{
    if (!IsRecognizedLiveChrTeam(chrType, teamType))
        return STICKY_ROUTE_NONE;

    if ((chrType == 0 && teamType == 1) || (chrType == 8 && teamType == 4))
        return STICKY_ROUTE_HOST_LIVE_TYPE;

    if (chrType == 1 && teamType == 2)
        return IsSunlightVowType(vowType) ? STICKY_ROUTE_SUNLIGHT : STICKY_ROUTE_WHITE_SUMMON;

    if (chrType == 2 && (teamType == 16 || teamType == 3))
    {
        // A normal red sign/red summon naturally uses the black phantom Chr/Team bucket.
        // Do not let the live Chr/Team bucket collapse red signs into red-eye invaders.
        if (IsNormalBlackSummonInvadeType(invadeType))
            return STICKY_ROUTE_RED_SUMMON;
        return STICKY_ROUTE_RED_INVADER;
    }

    if (chrType == 2 && teamType == 17)
    {
        // DS-Gadget notes Forest Hunter and Darkmoon share this Chr/Team pair, so use
        // InvadeType when it is available. Otherwise fall back to generic blue styling
        // rather than inventing a covenant/route identity.
        if (IsForestHunterInvadeType(invadeType))
            return STICKY_ROUTE_FOREST;
        if (IsGravelordBlueInvadeType(invadeType))
            return STICKY_ROUTE_GRAVELORD;
        if (IsDarkmoonBlueInvadeType(invadeType))
            return STICKY_ROUTE_DARKMOON;
        return STICKY_ROUTE_BLUE_GENERIC;
    }

    if (chrType == 2 && teamType == 18)
        return STICKY_ROUTE_DRAGON;

    return STICKY_ROUTE_NONE;
}

static int LiveChrTeamStyleKindForRow(const OverlayRow& row)
{
    return StyleKindFromLiveChrTeam(row.chrType, row.teamType, row.invadeType, row.vowType);
}

static int LiveChrTeamStyleKindForWorldRow(const WorldActorRow& row)
{
    return StyleKindFromLiveChrTeam(row.chrType, row.teamType, row.invadeType, row.vowType);
}

static bool LiveChrTeamStyleIs(const OverlayRow& row, int kind)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    return liveKind != STICKY_ROUTE_NONE && liveKind == kind;
}

static bool HasLiveChrTeamStyleOverride(const OverlayRow& row)
{
    return LiveChrTeamStyleKindForRow(row) != STICKY_ROUTE_NONE;
}

static const char* RoleTextFromWorldRow(const WorldActorRow& w)
{
    int liveKind = LiveChrTeamStyleKindForWorldRow(w);
    if (liveKind == STICKY_ROUTE_HOST_LIVE_TYPE)
        return "Host";
    if (liveKind == STICKY_ROUTE_SUNLIGHT)
        return "SunlightSummon";
    if (liveKind == STICKY_ROUTE_WHITE_SUMMON)
        return "WhiteSummon";
    if (liveKind == STICKY_ROUTE_DRAGON)
        return "DragonSummon";
    if (liveKind == STICKY_ROUTE_FOREST)
        return "ForestHunter";
    if (liveKind == STICKY_ROUTE_GRAVELORD)
        return "GravelordServant";
    if (liveKind == STICKY_ROUTE_DARKMOON)
        return "DarkmoonInvader";
    if (liveKind == STICKY_ROUTE_BLUE_GENERIC)
        return "BlueRoute";
    if (liveKind == STICKY_ROUTE_RED_SUMMON)
        return "RedSummon";
    if (liveKind == STICKY_ROUTE_RED_INVADER)
        return "RedInvader";

    if (w.isPhantom == 0)
        return "Host";

    // Entry-route roles first. Do not classify by covenant membership alone.
    if (IsSunlightSummonRoute(w.isPhantom, w.phantomType, w.invadeType, w.vowType))
        return "SunlightSummon";
    if (IsNormalWhiteSummonRoute(w.isPhantom, w.invadeType))
        return "WhiteSummon";
    if (IsDragonSummonRoute(w.isPhantom, w.invadeType))
        return "DragonSummon";
    if (IsForestHunterRoute(w.isPhantom, w.invadeType))
        return "ForestHunter";
    if (IsGravelordRouteFull(w.isPhantom, w.phantomType, w.invadeType, w.vowType))
        return "GravelordServant";
    if (IsDarkmoonRouteFull(w.isPhantom, w.phantomType, w.invadeType, w.vowType))
        return "DarkmoonInvader";
    if (IsRedSummonRoute(w.isPhantom, w.phantomType, w.redKind, w.invadeType))
        return "RedSummon";
    if (IsRedInvaderRoute(w.isPhantom, w.phantomType, w.redKind, w.invadeType))
        return "RedInvader";

    switch (PhantomTypeBase(w.phantomType))
    {
    case 1:
        return "WhiteSummon";
    case 2:
        return "BlueRoute";
    case 3:
        return "Red";
    default:
        return "Phantom";
    }
}

static bool IsInvaderWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_RED_INVADER;

    if (row.stickyRoute == STICKY_ROUTE_RED_SUMMON ||
        IsRedSummonRoute(row.isPhantom, row.phantomType, row.redKind, row.invadeType))
        return false;

    return row.stickyRoute == STICKY_ROUTE_RED_INVADER ||
        IsRedInvaderRoute(row.isPhantom, row.phantomType, row.redKind, row.invadeType);
}

static bool IsRedSummonWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_RED_SUMMON;

    return row.stickyRoute == STICKY_ROUTE_RED_SUMMON ||
        IsRedSummonRoute(row.isPhantom, row.phantomType, row.redKind, row.invadeType);
}

static bool IsDragonSummonWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_DRAGON;

    return row.stickyRoute == STICKY_ROUTE_DRAGON ||
        IsDragonSummonRoute(row.isPhantom, row.invadeType);
}

static bool IsGravelordBlueWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_GRAVELORD;

    return row.stickyRoute == STICKY_ROUTE_GRAVELORD ||
        IsGravelordRouteFull(row.isPhantom, row.phantomType, row.invadeType, row.vowType);
}

static bool IsDarkmoonBlueWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_DARKMOON;

    return row.stickyRoute == STICKY_ROUTE_DARKMOON ||
        IsDarkmoonRouteFull(row.isPhantom, row.phantomType, row.invadeType, row.vowType);
}

static bool IsGenericBlueAppearanceWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_BLUE_GENERIC;

    return row.stickyRoute == STICKY_ROUTE_BLUE_GENERIC ||
        (IsBlueAppearanceRoute(row.isPhantom, row.phantomType, row.invadeType) &&
            !IsGravelordBlueWorldRow(row) &&
            !IsDarkmoonBlueWorldRow(row));
}

static bool IsSunlightSummonWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_SUNLIGHT;

    return row.stickyRoute == STICKY_ROUTE_SUNLIGHT ||
        IsSunlightSummonRoute(row.isPhantom, row.phantomType, row.invadeType, row.vowType);
}

static bool IsWhiteSummonWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_WHITE_SUMMON;

    return row.stickyRoute == STICKY_ROUTE_WHITE_SUMMON ||
        (IsNormalWhiteSummonRoute(row.isPhantom, row.invadeType) && !IsSunlightSummonWorldRow(row)) ||
        (PhantomTypeBase(row.phantomType) == 1 && !IsSunlightSummonWorldRow(row));
}

static bool IsForestHunterWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind != STICKY_ROUTE_NONE)
        return liveKind == STICKY_ROUTE_FOREST;

    return row.stickyRoute == STICKY_ROUTE_FOREST ||
        IsForestHunterRoute(row.isPhantom, row.invadeType);
}

static COLORREF ColorForWorldRow(const OverlayRow& row)
{
    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind == STICKY_ROUTE_HOST_LIVE_TYPE)
        return RGB(255, 165, 60);      // host/human/hollow live type: orange text, black border

    if (IsSunlightSummonWorldRow(row))
        return RGB(255, 220, 40);      // Way of Sunlight white summon: yellow text, black border
    if (IsDragonSummonWorldRow(row))
        return RGB(210, 180, 120);     // Path of the Dragon summon: tan text, black border
    if (IsForestHunterWorldRow(row))
        return RGB(0, 0, 0);           // Cat Ring / Forest Hunter: black text, cyan border
    if (IsGravelordBlueWorldRow(row))
        return RGB(0, 0, 0);           // Gravelord Servant: black text, purple border
    if (IsDarkmoonBlueWorldRow(row))
        return RGB(0, 0, 0);           // Darkmoon: black text, blue border
    if (IsRedSummonWorldRow(row))
        return RGB(255, 70, 70);       // red summon: red text, black border
    if (IsInvaderWorldRow(row))
        return RGB(0, 0, 0);           // red invader: black text, red border
    if (IsWhiteSummonWorldRow(row))
        return RGB(245, 245, 245);     // normal white summon: white text, black border

    if (row.isPhantom == 0)
        return RGB(255, 165, 60);      // host: orange text, black border

    switch (PhantomTypeBase(row.phantomType))
    {
    case 1:
        return RGB(245, 245, 245);     // normal white summon: white text, black border
    case 2:
        return RGB(0, 0, 0);           // unknown blue route: black text, blue border fallback
    case 3:
        return RGB(255, 70, 70);       // uncertain red route: red text, black border
    default:
        return RGB(180, 180, 180);     // unknown phantom role
    }
}
static void LearnIdentityFromP2P(int playerNo, uint64_t steamId);

static bool GetLocalSteamNode(const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById, SteamNodeInfo& out)
{
    for (std::unordered_map<uint64_t, SteamNodeInfo>::const_iterator it = nodesById.begin(); it != nodesById.end(); ++it)
    {
        if (it->second.isLocal && it->second.steamId != 0)
        {
            out = it->second;
            return true;
        }
    }

    return false;
}


static bool AttachPingInfoToRow(OverlayRow& row, const PingInfo& ping)
{
    if (ping.ping < 0)
        return false;

    row.hasPing = true;
    row.status = ping.status;
    row.cid = ping.cid;
    row.ping = ping.ping;
    row.connection = ping.connection;
    return true;
}

static bool AttachPingFromSteamNode(OverlayRow& row, const SteamNodeInfo& node)
{
    if (node.hasPing)
        return AttachPingInfoToRow(row, node.ping);

    PingInfo cachedPing;
    ZeroMemory(&cachedPing, sizeof(cachedPing));

    if (FindCachedPingBySteamId(node.steamId, cachedPing))
        return AttachPingInfoToRow(row, cachedPing);

    if (FindCachedPingByName(node.name, cachedPing))
        return AttachPingInfoToRow(row, cachedPing);

    return false;
}

static bool NamesEqualNoCase(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return false;

    if (a == "Unknown" || b == "Unknown" || a == "(unknown)" || b == "(unknown)")
        return false;

    return lstrcmpiA(a.c_str(), b.c_str()) == 0;
}

static bool BindRemoteRowSteamIdByDisplayedName(OverlayRow& row, const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById)
{
    // Client-side host rows can miss the normal playerNo->SteamID bridge.
    // When that happens v60 could still show cached ping by name, but true-ping
    // could not attach because row.steamId stayed 0 (or worse, a transient local ID).
    // If this is a remote row, bind the row to a non-local Steam node whose persona
    // name matches the visible WORLD/playerParam name.
    if (row.isLocal)
        return false;

    bool needsBinding = (row.steamId == 0);
    if (g_localSteamId != 0 && row.steamId == g_localSteamId)
        needsBinding = true;

    if (!needsBinding)
        return false;

    for (std::unordered_map<uint64_t, SteamNodeInfo>::const_iterator it = nodesById.begin(); it != nodesById.end(); ++it)
    {
        const SteamNodeInfo& node = it->second;
        if (node.steamId == 0 || node.isLocal)
            continue;
        if (g_localSteamId != 0 && node.steamId == g_localSteamId)
            continue;

        if (NamesEqualNoCase(row.name, node.name) || NamesEqualNoCase(row.worldName, node.name))
        {
            uint64_t oldSteamId = row.steamId;
            row.steamId = node.steamId;
            if (!node.name.empty() && node.name != "(unknown)")
            {
                row.name = node.name;
                row.hasName = true;
                g_nameCacheBySteamId[node.steamId] = node.name;
            }
            if (g_truePingDebug)
            {
                WriteLogf(
                    "ROW_ID_NAME_BIND slot=%s pno=%d old=%08X%08X new=%08X%08X name=%s world=%s",
                    row.slotName ? row.slotName : "?",
                    row.playerNo,
                    (DWORD)(oldSteamId >> 32),
                    (DWORD)(oldSteamId & 0xFFFFFFFF),
                    (DWORD)(row.steamId >> 32),
                    (DWORD)(row.steamId & 0xFFFFFFFF),
                    row.name.c_str(),
                    row.worldName.c_str()
                );
            }
            return true;
        }
    }

    return false;
}

static bool AttachPingByDisplayedName(OverlayRow& row, const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById)
{
    // Even when cached ping is already present, still try to bind missing/incorrect
    // remote SteamID by name so true-ping can attach on the next ApplyTruePing pass.
    BindRemoteRowSteamIdByDisplayedName(row, nodesById);

    if (row.hasPing)
        return true;

    for (std::unordered_map<uint64_t, SteamNodeInfo>::const_iterator it = nodesById.begin(); it != nodesById.end(); ++it)
    {
        const SteamNodeInfo& node = it->second;

        if (NamesEqualNoCase(row.name, node.name) || NamesEqualNoCase(row.worldName, node.name))
        {
            if (!row.isLocal && node.steamId != 0 && !node.isLocal && (g_localSteamId == 0 || node.steamId != g_localSteamId))
            {
                row.steamId = node.steamId;
                g_nameCacheBySteamId[node.steamId] = node.name;
            }
            return AttachPingFromSteamNode(row, node);
        }
    }

    PingInfo cachedPing;
    ZeroMemory(&cachedPing, sizeof(cachedPing));

    if (FindCachedPingByName(row.name, cachedPing) || FindCachedPingByName(row.worldName, cachedPing))
        return AttachPingInfoToRow(row, cachedPing);

    return false;
}

static void LearnLocalSelfIdentity(const std::vector<WorldActorRow>& worldRows, const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById)
{
    SteamNodeInfo local;
    ZeroMemory(&local.ping, sizeof(local.ping));

    if (!GetLocalSteamNode(nodesById, local))
        return;

    for (size_t i = 0; i < worldRows.size(); i++)
    {
        const WorldActorRow& w = worldRows[i];
        if (!w.valid || w.slotIndex != 0 || !IsValidPlayerNo(w.playerNo))
            continue;

        LearnIdentityFromP2P(w.playerNo, local.steamId);
        if (!local.name.empty() && local.name != "(unknown)")
            g_nameCacheBySteamId[local.steamId] = local.name;
        return;
    }
}

static std::vector<OverlayRow> BuildOverlayRows(
    const std::vector<WorldActorRow>& worldRows,
    const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById
)
{
    std::vector<OverlayRow> rows;
    ULONGLONG now = GetTickCount64();

    for (size_t i = 0; i < worldRows.size(); i++)
    {
        const WorldActorRow& w = worldRows[i];

        if (!w.valid)
            continue;

        if (!IsValidPlayerNo(w.playerNo))
            continue;

        bool isLocal = (w.slotIndex == 0);
        if (g_hideLocal && isLocal)
            continue;

        PlayerIdentity id;
        bool hasIdentity = SnapshotIdentityForPlayerNo(w.playerNo, id);

        if (!hasIdentity && !g_showUnknownWorldRows)
            continue;

        OverlayRow row;

        row.slotName = w.slotName;
        row.slotIndex = w.slotIndex;
        row.chr = w.chr;
        row.playerNo = w.playerNo;
        row.isLocal = isLocal;
        row.isPhantom = w.isPhantom;
        row.phantomType = w.phantomType;
        row.redKind = w.redKind;
        row.invadeType = w.invadeType;
        row.vowType = w.vowType;
        row.chrType = w.chrType;
        row.teamType = w.teamType;
        row.playerParamChrType = w.playerParamChrType;
        row.steamId = hasIdentity ? id.steamId : 0;
        row.status = -1;
        row.cid = -1;
        row.ping = -1;
        row.hasTruePing = false;
        row.truePing = -1;
        row.trueRtt = -1;
        row.hasHp = w.hasHp;
        row.hp = w.hp;
        row.hpMax = w.hpMax;
        row.hpCurrentOffset = w.hpCurrentOffset;
        row.hpMaxOffset = w.hpMaxOffset;
        row.hpOneUntilTick = (w.hasHp && w.hp == 1) ? (now + g_hpOneLingerMs) : 0;
        row.connection = 0;
        row.learnedAgeMs = hasIdentity ? (now - id.lastSeenTick) : 0;
        row.stickyRoute = STICKY_ROUTE_NONE;
        row.stickyRouteAgeMs = 0;
        row.worldName = w.inGameName;
        row.name = w.inGameName;
        row.hasName = !row.name.empty();
        row.hasPing = false;

        // For the local/self row, prefer the local Steam node persona name.
        // The WORLD playerParam name is the character name, which is useful as worldName/debug,
        // but Dasmins' OBS bridge should display the same Steam-name style used for phantoms.
        if (isLocal)
        {
            SteamNodeInfo localNode;
            ZeroMemory(&localNode.ping, sizeof(localNode.ping));
            if (GetLocalSteamNode(nodesById, localNode))
            {
                if (!localNode.name.empty() && localNode.name != "(unknown)")
                {
                    row.name = localNode.name;
                    row.hasName = true;
                }
                if (localNode.steamId != 0)
                {
                    row.steamId = localNode.steamId;
                    g_nameCacheBySteamId[localNode.steamId] = localNode.name;
                }
                AttachPingFromSteamNode(row, localNode);
            }
        }

        if (hasIdentity)
        {
            std::unordered_map<uint64_t, SteamNodeInfo>::const_iterator nodeIt = nodesById.find(id.steamId);
            if (nodeIt != nodesById.end())
            {
                const SteamNodeInfo& node = nodeIt->second;
                // Prefer the Steam persona name over playerParam/worldName for every mapped row.
                // playerParam +0xA0 is often the character name, which is wrong for host display.
                if (!node.name.empty() && node.name != "(unknown)")
                {
                    row.name = node.name;
                    row.hasName = true;
                }

                AttachPingFromSteamNode(row, node);
            }
            else
            {
                std::unordered_map<uint64_t, std::string>::const_iterator cachedName = g_nameCacheBySteamId.find(id.steamId);
                if (cachedName != g_nameCacheBySteamId.end())
                {
                    // Cached Steam persona also beats playerParam/worldName.
                    if (!cachedName->second.empty() && cachedName->second != "(unknown)")
                    {
                        row.name = cachedName->second;
                        row.hasName = true;
                    }
                }
            }
        }

        // P2P identity can be missing on clients. Since playerParam +0xA0 now gives
        // the displayed name, use Steam-node name matching as a fallback for cached ping.
        AttachPingByDisplayedName(row, nodesById);

        if (row.name.empty())
        {
            char fallback[128];
            lstrcpyA(fallback, "Unknown");
            row.name = fallback;
        }

        ApplyStickyRouteClassification(row);

        rows.push_back(row);
    }

    // Committed name style is roster-lifetime based: once a visible phantom proves
    // its color, keep it only until that phantom row/name drops. Do not carry it
    // across loading screens, returns home, or host/local rows with the same name.
    PruneCommittedStyleCacheForVisibleRows(rows);

    std::sort(rows.begin(), rows.end(), [](const OverlayRow& a, const OverlayRow& b) {
        if (a.isLocal != b.isLocal)
            return a.isLocal > b.isLocal;
        if (a.playerNo != b.playerNo)
            return a.playerNo < b.playerNo;
        return a.slotIndex < b.slotIndex;
        });

    return rows;
}

static void HexBytesForDump(uintptr_t address, int byteCount, char* out, int outSize)
{
    if (!out || outSize <= 0)
        return;

    out[0] = 0;
    if (!LooksLikePointer(address) || byteCount <= 0)
        return;

    BYTE bytes[128];
    if (byteCount > (int)sizeof(bytes))
        byteCount = (int)sizeof(bytes);

    if (!SafeReadBytes(address, bytes, byteCount))
        return;

    static const char* hex = "0123456789ABCDEF";
    int pos = 0;
    for (int i = 0; i < byteCount && pos + 4 < outSize; i++)
    {
        BYTE b = bytes[i];
        out[pos++] = hex[(b >> 4) & 0xF];
        out[pos++] = hex[b & 0xF];
        if (i + 1 < byteCount)
            out[pos++] = ' ';
    }
    out[pos] = 0;
}


static void HexBytesLimited(const BYTE* data, uint32_t size, char* out, int outSize);

static const char* MctpKindText(uint16_t kind)
{
    switch (kind)
    {
    case MCTP_HELLO: return "HELLO";
    case MCTP_HELLO_ACK: return "HELLO_ACK";
    case MCTP_PING: return "PING";
    case MCTP_PONG: return "PONG";
    default: return "UNKNOWN";
    }
}

static uint64_t QpcNow()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
}

static int QpcDeltaMs(uint64_t newer, uint64_t older)
{
    if (g_qpcFrequency.QuadPart <= 0 || newer < older)
        return -1;
    uint64_t delta = newer - older;
    return (int)((delta * 1000ULL) / (uint64_t)g_qpcFrequency.QuadPart);
}

static void EnableTruePingTimerPeriod()
{
    if (!g_truePingUseHighResTimer || g_truePingTimerPeriodSet)
        return;

    HMODULE winmm = LoadLibraryA("winmm.dll");
    if (!winmm)
    {
        if (g_truePingDebug)
            WriteLogLine("TRUEPING timer: winmm.dll unavailable; using normal scheduler timer.");
        return;
    }

    g_timeBeginPeriodFn = (TimePeriodFn)GetProcAddress(winmm, "timeBeginPeriod");
    g_timeEndPeriodFn = (TimePeriodFn)GetProcAddress(winmm, "timeEndPeriod");
    if (!g_timeBeginPeriodFn || !g_timeEndPeriodFn)
    {
        if (g_truePingDebug)
            WriteLogLine("TRUEPING timer: timeBeginPeriod/timeEndPeriod unavailable; using normal scheduler timer.");
        return;
    }

    UINT res = g_timeBeginPeriodFn(1);
    if (res == 0)
    {
        g_truePingTimerPeriodSet = true;
        if (g_truePingDebug)
            WriteLogLine("TRUEPING timer: enabled 1ms scheduler period.");
    }
    else if (g_truePingDebug)
    {
        WriteLogf("TRUEPING timer: timeBeginPeriod(1) failed res=%u", res);
    }
}

static void DisableTruePingTimerPeriod()
{
    if (g_truePingTimerPeriodSet && g_timeEndPeriodFn)
    {
        g_timeEndPeriodFn(1);
        g_truePingTimerPeriodSet = false;
        if (g_truePingDebug)
            WriteLogLine("TRUEPING timer: restored scheduler period.");
    }
}

static int ComputeTruePingDisplayValue(TruePingPeerInfo& info, int rawPingMs)
{
    if (rawPingMs < 0)
        return rawPingMs;

    int window = g_truePingBestWindow;
    if (window < 1) window = 1;
    if (window > 16) window = 16;

    info.sampleRing[info.sampleRingIndex % 16] = rawPingMs;
    info.sampleRingIndex = (info.sampleRingIndex + 1) % 16;
    if (info.sampleRingCount < 16)
        info.sampleRingCount++;

    if (g_truePingDisplayMode == 2 && window > 1)
    {
        int count = info.sampleRingCount < window ? info.sampleRingCount : window;
        int best = rawPingMs;
        for (int i = 0; i < count; i++)
        {
            int idx = (info.sampleRingIndex - 1 - i + 16) % 16;
            int v = info.sampleRing[idx];
            if (v >= 0 && v < best)
                best = v;
        }
        return best;
    }

    if (g_truePingDisplayMode == 1 && g_truePingSmoothWeight > 1 && info.sampleCount > 0)
    {
        int oldDisplay = info.displayPingMs;
        return ((oldDisplay * (g_truePingSmoothWeight - 1)) + rawPingMs + (g_truePingSmoothWeight / 2)) / g_truePingSmoothWeight;
    }

    return rawPingMs;
}

static bool ResolveSteamNetworking(bool logIt)
{
    if (g_steamNetworkingIface && g_steamSendP2PPacket && g_steamReadP2PPacket)
        return true;

    HMODULE steamApi = GetModuleHandleA("steam_api.dll");
    if (!steamApi)
    {
        if (logIt)
            WriteLogLine("TRUEPING resolve failed: steam_api.dll not loaded yet.");
        return false;
    }

    BYTE* thunk = (BYTE*)steamApi + 0x2F70;
    BYTE first = 0;
    __try { first = thunk[0]; }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (logIt) WriteLogLine("TRUEPING resolve failed: could not read steam_api.dll+2F70.");
        return false;
    }

    if (first != 0xA1 && logIt)
        WriteLogf("TRUEPING warning: steam_api.dll+2F70 first byte is %02X, expected A1 from MPChan notes. Trying anyway.", first);

    SteamNetworkingThunkFn thunkFn = (SteamNetworkingThunkFn)thunk;
    void* iface = NULL;
    __try { iface = thunkFn(); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (logIt) WriteLogLine("TRUEPING resolve failed: SteamNetworking thunk call crashed.");
        return false;
    }

    if (!LooksLikePointer((uintptr_t)iface))
    {
        if (logIt) WriteLogf("TRUEPING resolve failed: SteamNetworking iface pointer invalid: %08X", (DWORD)(uintptr_t)iface);
        return false;
    }

    uintptr_t* vtbl = NULL;
    __try { vtbl = *(uintptr_t**)iface; }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (logIt) WriteLogLine("TRUEPING resolve failed: could not read SteamNetworking vtable.");
        return false;
    }

    if (!LooksLikePointer((uintptr_t)vtbl))
    {
        if (logIt) WriteLogf("TRUEPING resolve failed: SteamNetworking vtable invalid: %08X", (DWORD)(uintptr_t)vtbl);
        return false;
    }

    SteamSendP2PPacketFn sendFn = NULL;
    SteamIsP2PPacketAvailableFn availFn = NULL;
    SteamReadP2PPacketFn readFn = NULL;
    __try
    {
        sendFn = (SteamSendP2PPacketFn)vtbl[0];
        availFn = (SteamIsP2PPacketAvailableFn)vtbl[1];
        readFn = (SteamReadP2PPacketFn)vtbl[2];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (logIt) WriteLogLine("TRUEPING resolve failed: could not read SteamNetworking vtable slots.");
        return false;
    }

    if (!LooksLikePointer((uintptr_t)sendFn) || !LooksLikePointer((uintptr_t)readFn))
    {
        if (logIt) WriteLogf("TRUEPING resolve failed: bad vtable funcs send=%08X avail=%08X read=%08X", (DWORD)(uintptr_t)sendFn, (DWORD)(uintptr_t)availFn, (DWORD)(uintptr_t)readFn);
        return false;
    }

    g_steamNetworkingIface = iface;
    g_steamSendP2PPacket = sendFn;
    g_steamIsP2PPacketAvailable = availFn;
    g_steamReadP2PPacket = readFn;

    if (logIt)
        WriteLogf("TRUEPING resolved steam_api=%08X thunk=%08X iface=%08X vtbl=%08X send=%08X avail=%08X read=%08X channel=%d sendType=%d", (DWORD)(uintptr_t)steamApi, (DWORD)(uintptr_t)thunk, (DWORD)(uintptr_t)iface, (DWORD)(uintptr_t)vtbl, (DWORD)(uintptr_t)sendFn, (DWORD)(uintptr_t)availFn, (DWORD)(uintptr_t)readFn, g_truePingChannel, g_truePingSendType);
    return true;
}

static bool SendMctpPacket(uint64_t targetSteamId, uint16_t kind, uint32_t nonce, uint64_t qpc)
{
    if (!g_truePingEnabled || !g_truePingSendEnabled || targetSteamId == 0 || !ResolveSteamNetworking(false))
        return false;

    MctdePingPacket pkt;
    ZeroMemory(&pkt, sizeof(pkt));
    pkt.magic = MCTP_MAGIC;
    pkt.version = MCTP_VERSION;
    pkt.kind = kind;
    pkt.nonce = nonce;
    pkt.senderSteamId = 0;
    pkt.qpc = qpc;

    if (g_truePingVerbose)
        WriteLogf("TRUEPING_PRE_SEND kind=%s target=%08X%08X nonce=%u ch=%d type=%d iface=%08X send=%08X", MctpKindText(kind), (DWORD)(targetSteamId >> 32), (DWORD)(targetSteamId & 0xFFFFFFFF), nonce, g_truePingChannel, g_truePingSendType, (DWORD)(uintptr_t)g_steamNetworkingIface, (DWORD)(uintptr_t)g_steamSendP2PPacket);

    bool ok = false;
    __try { ok = g_steamSendP2PPacket(g_steamNetworkingIface, targetSteamId, &pkt, (uint32_t)sizeof(pkt), g_truePingSendType, g_truePingChannel) ? true : false; }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WriteLogf("TRUEPING_SEND exception target=%08X%08X kind=%s", (DWORD)(targetSteamId >> 32), (DWORD)(targetSteamId & 0xFFFFFFFF), MctpKindText(kind));
        return false;
    }

    if (g_truePingVerbose || !ok)
    {
        LONG seq = InterlockedIncrement(&g_truePingSeq);
        if (g_truePingVerbose || !ok)
            WriteLogf("TRUEPING_SEND seq=%ld kind=%s target=%08X%08X nonce=%u ok=%d ch=%d type=%d", seq, MctpKindText(kind), (DWORD)(targetSteamId >> 32), (DWORD)(targetSteamId & 0xFFFFFFFF), nonce, ok ? 1 : 0, g_truePingChannel, g_truePingSendType);
    }
    return ok;
}

static void MarkTruePingHandshake(uint64_t steamId, bool ackSeen)
{
    if (!g_truePingLockReady || steamId == 0)
        return;
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_truePingLock);
    TruePingPeerInfo& info = g_truePingBySteamId[steamId];
    bool wasHandshaken = info.handshaken;
    info.handshaken = true;
    info.lastSeenTick = now;
    LeaveCriticalSection(&g_truePingLock);
    if (g_truePingDebug && (!wasHandshaken || ackSeen))
        WriteLogf("TRUEPING_HANDSHAKE steam=%08X%08X ack=%d was=%d", (DWORD)(steamId >> 32), (DWORD)(steamId & 0xFFFFFFFF), ackSeen ? 1 : 0, wasHandshaken ? 1 : 0);
}

static void HandleMctpPacket(uint64_t remoteSteamId, const MctdePingPacket& pkt)
{
    if (pkt.magic != MCTP_MAGIC || pkt.version != MCTP_VERSION)
        return;
    ULONGLONG nowTick = GetTickCount64();
    if (g_truePingVerbose)
    {
        LONG seq = InterlockedIncrement(&g_truePingSeq);
        if (g_truePingVerbose)
            WriteLogf("TRUEPING_RECV seq=%ld kind=%s steam=%08X%08X nonce=%u qpc=%I64u bytes=%d", seq, MctpKindText(pkt.kind), (DWORD)(remoteSteamId >> 32), (DWORD)(remoteSteamId & 0xFFFFFFFF), pkt.nonce, pkt.qpc, (int)sizeof(pkt));
    }
    if (pkt.kind == MCTP_HELLO)
    {
        MarkTruePingHandshake(remoteSteamId, false);
        SendMctpPacket(remoteSteamId, MCTP_HELLO_ACK, pkt.nonce, pkt.qpc);
        return;
    }
    if (pkt.kind == MCTP_HELLO_ACK)
    {
        MarkTruePingHandshake(remoteSteamId, true);
        return;
    }
    if (pkt.kind == MCTP_PING)
    {
        MarkTruePingHandshake(remoteSteamId, false);
        SendMctpPacket(remoteSteamId, MCTP_PONG, pkt.nonce, pkt.qpc);
        return;
    }
    if (pkt.kind == MCTP_PONG)
    {
        if (!g_truePingLockReady)
            return;
        uint64_t nowQpc = QpcNow();
        int rttMs = -1;
        int rawPingMs = -1;
        int pingMs = -1;
        bool accepted = false;
        EnterCriticalSection(&g_truePingLock);
        std::unordered_map<uint64_t, TruePingPeerInfo>::iterator it = g_truePingBySteamId.find(remoteSteamId);
        if (it != g_truePingBySteamId.end() && it->second.lastNonce == pkt.nonce && it->second.lastPingQpc != 0)
        {
            rttMs = QpcDeltaMs(nowQpc, it->second.lastPingQpc);
            if (rttMs >= 0 && rttMs < 60000)
            {
                rawPingMs = rttMs / 2;
                pingMs = ComputeTruePingDisplayValue(it->second, rawPingMs);
                it->second.rttMs = rttMs;
                it->second.rawPingMs = rawPingMs;
                it->second.displayPingMs = pingMs;
                it->second.sampleCount++;
                it->second.pingMs = pingMs;
                it->second.lastSeenTick = nowTick;
                it->second.handshaken = true;
                accepted = true;
            }
        }
        LeaveCriticalSection(&g_truePingLock);
        if (g_truePingVerbose || !accepted)
            WriteLogf("TRUEPING_RESULT steam=%08X%08X nonce=%u accepted=%d rtt=%d rawPing=%d displayPing=%d", (DWORD)(remoteSteamId >> 32), (DWORD)(remoteSteamId & 0xFFFFFFFF), pkt.nonce, accepted ? 1 : 0, rttMs, rawPingMs, pingMs);
    }
}

static bool ReadOneMctpPacket()
{
    if (!g_truePingReceiveEnabled || !ResolveSteamNetworking(false))
        return false;
    uint32_t availSize = 0;
    bool available = false;
    __try { available = (g_steamIsP2PPacketAvailable && g_steamIsP2PPacketAvailable(g_steamNetworkingIface, &availSize, g_truePingChannel)) ? true : false; }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WriteLogLine("TRUEPING_AVAIL exception from IsP2PPacketAvailable; disabling true-ping receive for this run.");
        g_truePingReceiveEnabled = false;
        return false;
    }
    if (!available || availSize == 0)
        return false;

    BYTE buffer[256];
    ZeroMemory(buffer, sizeof(buffer));
    uint32_t bytesRead = 0;
    uint64_t remoteSteamId = 0;
    bool ok = false;
    __try { ok = g_steamReadP2PPacket(g_steamNetworkingIface, buffer, sizeof(buffer), &bytesRead, &remoteSteamId, g_truePingChannel) ? true : false; }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WriteLogLine("TRUEPING_RECV exception from ReadP2PPacket; disabling true ping for this run.");
        g_truePingReceiveEnabled = false;
        return false;
    }
    if (!ok)
        return false;
    if (bytesRead == sizeof(MctdePingPacket))
    {
        MctdePingPacket pkt;
        memcpy(&pkt, buffer, sizeof(pkt));
        HandleMctpPacket(remoteSteamId, pkt);
    }
    else if (g_truePingVerbose)
    {
        char hexLine[256];
        HexBytesLimited(buffer, bytesRead, hexLine, sizeof(hexLine));
        WriteLogf("TRUEPING_RECV_NON_MCTP steam=%08X%08X bytes=%u ch=%d hex=%s", (DWORD)(remoteSteamId >> 32), (DWORD)(remoteSteamId & 0xFFFFFFFF), bytesRead, g_truePingChannel, hexLine);
    }
    return true;
}


static void AddUniqueSteamTarget(std::vector<uint64_t>& targets, uint64_t steamId)
{
    if (steamId == 0)
        return;
    if (g_localSteamId != 0 && steamId == g_localSteamId)
        return;
    for (size_t i = 0; i < targets.size(); i++)
    {
        if (targets[i] == steamId)
            return;
    }
    targets.push_back(steamId);
}

static void AddRecentP2PRemoteTargets(std::vector<uint64_t>& targets)
{
    if (!g_identityLockReady)
        return;

    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_identityLock);
    for (std::unordered_map<uint64_t, ULONGLONG>::const_iterator it = g_recentP2PRemoteSteamIds.begin(); it != g_recentP2PRemoteSteamIds.end(); ++it)
    {
        uint64_t steamId = it->first;
        ULONGLONG seenTick = it->second;
        if (steamId == 0)
            continue;
        if (g_localSteamId != 0 && steamId == g_localSteamId)
            continue;
        if (seenTick == 0 || now - seenTick > RECENT_P2P_REMOTE_TTL_MS)
            continue;

        AddUniqueSteamTarget(targets, steamId);
    }
    LeaveCriticalSection(&g_identityLock);
}

static void UpdateTruePingKnownRemoteSteamNodes(const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById)
{
    if (!g_truePingEnabled || !g_truePingLockReady)
        return;

    EnterCriticalSection(&g_truePingLock);
    g_truePingKnownRemoteTick = GetTickCount64();
    for (std::unordered_map<uint64_t, SteamNodeInfo>::const_iterator it = nodesById.begin(); it != nodesById.end(); ++it)
    {
        const SteamNodeInfo& node = it->second;
        if (node.steamId == 0 || node.isLocal)
            continue;
        if (g_localSteamId != 0 && node.steamId == g_localSteamId)
            continue;
        if (!node.name.empty() && node.name != "(unknown)")
            g_truePingKnownRemoteNames[node.steamId] = node.name;
        else if (g_truePingKnownRemoteNames.find(node.steamId) == g_truePingKnownRemoteNames.end())
            g_truePingKnownRemoteNames[node.steamId] = "";
    }
    LeaveCriticalSection(&g_truePingLock);
}

static bool GetTruePingForDisplayedName(const std::string& name, const std::string& worldName, uint64_t& outSteamId, int& outPingMs, int& outRttMs)
{
    outSteamId = 0;
    outPingMs = -1;
    outRttMs = -1;

    if (!g_truePingEnabled || !g_truePingPreferOverlay || !g_truePingLockReady)
        return false;

    ULONGLONG now = GetTickCount64();
    bool ok = false;

    EnterCriticalSection(&g_truePingLock);
    for (std::unordered_map<uint64_t, std::string>::const_iterator it = g_truePingKnownRemoteNames.begin(); it != g_truePingKnownRemoteNames.end(); ++it)
    {
        uint64_t steamId = it->first;
        if (steamId == 0)
            continue;
        if (g_localSteamId != 0 && steamId == g_localSteamId)
            continue;

        const std::string& knownName = it->second;
        if (!NamesEqualNoCase(name, knownName) && !NamesEqualNoCase(worldName, knownName))
            continue;

        std::unordered_map<uint64_t, TruePingPeerInfo>::iterator pingIt = g_truePingBySteamId.find(steamId);
        if (pingIt == g_truePingBySteamId.end())
            continue;

        const TruePingPeerInfo& info = pingIt->second;
        if (info.handshaken && info.sampleCount > 0 && info.pingMs >= 0 && info.lastSeenTick != 0 && now - info.lastSeenTick <= (ULONGLONG)g_truePingStaleMs)
        {
            outSteamId = steamId;
            outPingMs = info.pingMs;
            outRttMs = info.rttMs;
            ok = true;
            break;
        }
    }
    LeaveCriticalSection(&g_truePingLock);

    return ok;
}

static void SnapshotTruePingTargets(std::vector<uint64_t>& targets)
{
    targets.clear();

    // Primary source: overlay rows that already have a reliable SteamID.
    if (g_rowsLockReady)
    {
        EnterCriticalSection(&g_rowsLock);
        for (size_t i = 0; i < g_rows.size(); i++)
        {
            const OverlayRow& row = g_rows[i];
            if (row.isLocal || row.steamId == 0)
                continue;
            // Never send custom side-channel packets to our own SteamID, even if a transient
            // playerNo/SteamID learning glitch maps the local node onto a remote row.
            AddUniqueSteamTarget(targets, row.steamId);
        }
        LeaveCriticalSection(&g_rowsLock);
    }

    // v62: secondary source: raw Steam node list. Red phantoms/clients can sometimes
    // fail to bind the host WORLD row to a SteamID early enough, even though the Steam
    // node/cached ping is visible. Ping every known non-local Steam node so the host can
    // handshake even before the row identity bridge is perfect.
    if (g_truePingLockReady)
    {
        EnterCriticalSection(&g_truePingLock);
        for (std::unordered_map<uint64_t, std::string>::const_iterator it = g_truePingKnownRemoteNames.begin(); it != g_truePingKnownRemoteNames.end(); ++it)
            AddUniqueSteamTarget(targets, it->first);
        LeaveCriticalSection(&g_truePingLock);
    }

    AddRecentP2PRemoteTargets(targets);
}

static void TickTruePingSends()
{
    if (!g_truePingEnabled || !g_truePingSendEnabled || !g_truePingLockReady)
        return;
    std::vector<uint64_t> targets;
    SnapshotTruePingTargets(targets);
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < targets.size(); i++)
    {
        uint64_t steamId = targets[i];
        bool sendHello = false;
        bool sendPing = false;
        uint32_t nonce = 0;
        uint64_t qpc = 0;
        EnterCriticalSection(&g_truePingLock);
        TruePingPeerInfo& info = g_truePingBySteamId[steamId];
        if (!info.handshaken)
        {
            if (info.lastHelloTick == 0 || now - info.lastHelloTick >= (ULONGLONG)g_truePingHelloMs)
            {
                info.lastHelloTick = now;
                nonce = (uint32_t)InterlockedIncrement(&g_truePingNonce);
                qpc = QpcNow();
                sendHello = true;
            }
        }
        else
        {
            bool stale = (info.lastSeenTick == 0 || now - info.lastSeenTick > (ULONGLONG)g_truePingStaleMs);
            if (info.lastPingTick == 0 || now - info.lastPingTick >= (ULONGLONG)g_truePingSendMs || stale)
            {
                info.lastPingTick = now;
                info.lastNonce = (uint32_t)InterlockedIncrement(&g_truePingNonce);
                info.lastPingQpc = QpcNow();
                nonce = info.lastNonce;
                qpc = info.lastPingQpc;
                sendPing = true;
            }
        }
        LeaveCriticalSection(&g_truePingLock);
        if (sendHello)
            SendMctpPacket(steamId, MCTP_HELLO, nonce, qpc);
        else if (sendPing)
            SendMctpPacket(steamId, MCTP_PING, nonce, qpc);
    }
}

static bool GetTruePingForSteamId(uint64_t steamId, int& outPingMs, int& outRttMs)
{
    outPingMs = -1;
    outRttMs = -1;
    if (!g_truePingEnabled || !g_truePingPreferOverlay || !g_truePingLockReady || steamId == 0)
        return false;
    ULONGLONG now = GetTickCount64();
    bool ok = false;
    EnterCriticalSection(&g_truePingLock);
    std::unordered_map<uint64_t, TruePingPeerInfo>::iterator it = g_truePingBySteamId.find(steamId);
    if (it != g_truePingBySteamId.end())
    {
        const TruePingPeerInfo& info = it->second;
        if (info.handshaken && info.sampleCount > 0 && info.pingMs >= 0 && info.lastSeenTick != 0 && now - info.lastSeenTick <= (ULONGLONG)g_truePingStaleMs)
        {
            outPingMs = info.pingMs;
            outRttMs = info.rttMs;
            ok = true;
        }
    }
    LeaveCriticalSection(&g_truePingLock);
    return ok;
}

static void ApplyTruePingToRows(std::vector<OverlayRow>& rows)
{
    if (!g_truePingEnabled || !g_truePingPreferOverlay)
        return;
    for (size_t i = 0; i < rows.size(); i++)
    {
        OverlayRow& row = rows[i];
        int pingMs = -1;
        int rttMs = -1;
        bool matched = false;

        if (row.steamId != 0 && GetTruePingForSteamId(row.steamId, pingMs, rttMs))
        {
            matched = true;
        }
        else
        {
            // v62: red phantoms can see the host's cached Steam node but fail to bind
            // the host WORLD row to its SteamID. If true-ping has already handshaken
            // with a known remote Steam node, attach by visible Steam/persona name.
            uint64_t matchedSteamId = 0;
            if (GetTruePingForDisplayedName(row.name, row.worldName, matchedSteamId, pingMs, rttMs))
            {
                row.steamId = matchedSteamId;
                matched = true;
                if (g_truePingDebug)
                {
                    WriteLogf(
                        "TRUEPING_NAME_ATTACH slot=%s pno=%d steam=%08X%08X name=%s world=%s ping=%d rtt=%d",
                        row.slotName ? row.slotName : "?",
                        row.playerNo,
                        (DWORD)(matchedSteamId >> 32),
                        (DWORD)(matchedSteamId & 0xFFFFFFFF),
                        row.name.c_str(),
                        row.worldName.c_str(),
                        pingMs,
                        rttMs
                    );
                }
            }
        }

        if (matched)
        {
            row.hasTruePing = true;
            row.truePing = pingMs;
            row.trueRtt = rttMs;
            row.hasPing = true;
            row.ping = pingMs;
            row.status = 9001;
        }
    }
}

static bool IsUsableDisplayName(const std::string& name)
{
    return !name.empty() && name != "Unknown" && name != "(unknown)";
}

static const OverlayRow* FindPreviousOverlayRow(const OverlayRow& row, const std::vector<OverlayRow>& previousRows)
{
    if (row.steamId != 0)
    {
        for (size_t i = 0; i < previousRows.size(); i++)
        {
            const OverlayRow& oldRow = previousRows[i];
            if (oldRow.steamId == row.steamId && oldRow.isLocal == row.isLocal)
                return &oldRow;
        }
    }

    if (IsValidPlayerNo(row.playerNo))
    {
        for (size_t i = 0; i < previousRows.size(); i++)
        {
            const OverlayRow& oldRow = previousRows[i];
            if (oldRow.playerNo == row.playerNo && oldRow.slotIndex == row.slotIndex && oldRow.isLocal == row.isLocal)
                return &oldRow;
        }
    }

    if (row.slotIndex >= 0)
    {
        for (size_t i = 0; i < previousRows.size(); i++)
        {
            const OverlayRow& oldRow = previousRows[i];
            if (oldRow.slotIndex == row.slotIndex && oldRow.isLocal == row.isLocal)
                return &oldRow;
        }
    }

    return NULL;
}

static bool ShouldCarryPreviousName(const OverlayRow& row, const OverlayRow& oldRow)
{
    if (!IsUsableDisplayName(oldRow.name))
        return false;

    if (!IsUsableDisplayName(row.name))
        return true;

    // When a Steam identity is stable, do not let the row flicker back to the
    // character/world name for a single frame just because the Steam node blinked.
    if (row.steamId != 0 && row.steamId == oldRow.steamId)
    {
        bool newLooksLikeWorldName = IsUsableDisplayName(row.worldName) && NamesEqualNoCase(row.name, row.worldName);
        bool oldLooksLikePersona = !IsUsableDisplayName(oldRow.worldName) || !NamesEqualNoCase(oldRow.name, oldRow.worldName);
        if (newLooksLikeWorldName && oldLooksLikePersona)
            return true;
    }

    return false;
}

static bool OverlayRosterChanged(const std::vector<OverlayRow>& rows, const std::vector<OverlayRow>& previousRows)
{
    if (rows.size() != previousRows.size())
        return true;

    for (size_t i = 0; i < rows.size(); i++)
    {
        const OverlayRow& row = rows[i];
        bool found = false;

        for (size_t j = 0; j < previousRows.size(); j++)
        {
            const OverlayRow& oldRow = previousRows[j];
            if (row.slotIndex == oldRow.slotIndex &&
                row.playerNo == oldRow.playerNo &&
                row.isLocal == oldRow.isLocal &&
                row.chr == oldRow.chr)
            {
                found = true;
                break;
            }
        }

        if (!found)
            return true;
    }

    return false;
}

// Stable per-player key for tracking across refreshes: SteamID if known, else name, else slot.
static std::string OverlayRowKey(const OverlayRow& r)
{
    if (r.steamId != 0)
        return std::string("s") + std::to_string((unsigned long long)r.steamId);
    if (!r.name.empty())
        return std::string("n") + r.name;
    return std::string("p") + std::to_string(r.playerNo);
}

// Turn players who left the session WITHOUT dying into persistent "Disconnected" placeholder
// rows. A placeholder is kept until that player reconnects (their real row returns) or a
// newcomer joins (which consumes the oldest placeholder, "replacing" it). Pure overlay state:
// `previousRows` is last frame's g_rows (which already carries any placeholders), `rows` is the
// freshly built live roster; surviving placeholders are appended to `rows`.
static void ApplyDisconnectedRows(std::vector<OverlayRow>& rows, const std::vector<OverlayRow>& previousRows)
{
    if (!g_showDisconnected)
        return;

    std::unordered_set<std::string> liveKeys;
    for (size_t i = 0; i < rows.size(); i++)
        liveKeys.insert(OverlayRowKey(rows[i]));

    // Carry forward existing placeholders, dropping any whose player is live again.
    std::vector<OverlayRow> placeholders;
    std::unordered_set<std::string> placeholderKeys;
    for (size_t i = 0; i < previousRows.size(); i++)
    {
        if (!previousRows[i].disconnected)
            continue;
        std::string k = OverlayRowKey(previousRows[i]);
        if (liveKeys.count(k) || placeholderKeys.count(k))
            continue;
        placeholders.push_back(previousRows[i]);
        placeholderKeys.insert(k);
    }

    // New departures: previously-live (real) players no longer present and not killed.
    std::unordered_set<std::string> prevRealKeys;
    for (size_t i = 0; i < previousRows.size(); i++)
    {
        const OverlayRow& pr = previousRows[i];
        if (pr.disconnected)
            continue;
        std::string k = OverlayRowKey(pr);
        prevRealKeys.insert(k);
        if (liveKeys.count(k) || placeholderKeys.count(k))
            continue;
        if (pr.isLocal || pr.name.empty())
            continue;                       // never placeholder the local player / nameless rows
        if (pr.hasHp && pr.hp <= 0)
            continue;                       // died -> not a disconnect

        OverlayRow ph;
        ph.name = pr.name;
        ph.worldName = pr.worldName;
        ph.steamId = pr.steamId;
        ph.playerNo = pr.playerNo;
        ph.hasName = pr.hasName;
        ph.disconnected = true;
        placeholders.push_back(ph);
        placeholderKeys.insert(k);
    }

    // Each newcomer (live now, not previously live) consumes the oldest placeholder.
    int newcomers = 0;
    for (size_t i = 0; i < rows.size(); i++)
        if (!prevRealKeys.count(OverlayRowKey(rows[i])))
            newcomers++;
    while (newcomers > 0 && !placeholders.empty())
    {
        placeholders.erase(placeholders.begin());
        newcomers--;
    }

    for (size_t i = 0; i < placeholders.size(); i++)
        rows.push_back(placeholders[i]);
}

static void StabilizeRowsWithPrevious(std::vector<OverlayRow>& rows, const std::vector<OverlayRow>& previousRows, bool rosterChanged)
{
    if (previousRows.empty())
        return;

    for (size_t i = 0; i < rows.size(); i++)
    {
        OverlayRow& row = rows[i];
        const OverlayRow* old = FindPreviousOverlayRow(row, previousRows);
        if (!old)
            continue;

        if (row.steamId == 0 && old->steamId != 0)
            row.steamId = old->steamId;

        if ((!rosterChanged && IsUsableDisplayName(old->name)) || ShouldCarryPreviousName(row, *old))
        {
            row.name = old->name;
            row.hasName = old->hasName;
        }

        if (!row.hasHp && old->hasHp)
        {
            row.hasHp = true;
            row.hp = old->hp;
            row.hpMax = old->hpMax;
            row.hpCurrentOffset = old->hpCurrentOffset;
            row.hpMaxOffset = old->hpMaxOffset;
        }

        if (old->hpOneUntilTick > row.hpOneUntilTick)
            row.hpOneUntilTick = old->hpOneUntilTick;

        if (!row.hasPing || (old->hasTruePing && !row.hasTruePing))
        {
            row.hasPing = old->hasPing;
            row.ping = old->ping;
            row.status = old->status;
            row.cid = old->cid;
            row.connection = old->connection;
            row.hasTruePing = old->hasTruePing;
            row.truePing = old->truePing;
            row.trueRtt = old->trueRtt;
        }
    }
}

static void DumpDebugRows(const std::vector<OverlayRow>& overlayRows, const std::vector<WorldActorRow>& worldRows, const std::unordered_map<uint64_t, SteamNodeInfo>& nodesById)
{
    LONG dumpNumber = InterlockedIncrement(&g_dumpCounter);
    bool doDump = g_dumpOverlayData && (dumpNumber <= 15 || (dumpNumber % 10) == 0);
    if (!doDump)
        return;

    WriteLogf("---- Overlay data dump %ld ----", dumpNumber);
    WriteLogf("OVERLAY rows=%d WORLD rows=%d STEAM nodes=%d", (int)overlayRows.size(), (int)worldRows.size(), (int)nodesById.size());

    for (size_t i = 0; i < worldRows.size(); i++)
    {
        const WorldActorRow& w = worldRows[i];

        // v59 join-safe side-channel: when the WorldChr slot is not valid yet, do not
        // format role/name fields at all. This keeps early load debug dumps from
        // touching stale role/name data and also makes it obvious in logs that
        // the game has not produced real world rows yet.
        if (!w.valid)
        {
            WriteLogf(
                "WORLD[%s] valid=0 chr=%08X pparam=%08X pno=%d phantomData=%08X hp=%d/%d",
                w.slotName,
                (DWORD)w.chr,
                (DWORD)w.playerParam,
                w.playerNo,
                (DWORD)w.phantomData,
                w.hasHp ? w.hp : -1,
                w.hasHp ? w.hpMax : -1
            );
            continue;
        }

        WriteLogf(
            "WORLD[%s] valid=%d chr=%08X pparam=%08X pno=%d phantomData=%08X isPh=%d phType=%d redKind=%d invadeType=%d vowType=%d chrType=%d teamType=%d ppChrType=%d hp=%d/%d hpOff=%d/%d roleGuess=%s pparamName=%s",
            w.slotName,
            w.valid ? 1 : 0,
            (DWORD)w.chr,
            (DWORD)w.playerParam,
            w.playerNo,
            (DWORD)w.phantomData,
            w.isPhantom,
            w.phantomType,
            w.redKind,
            w.invadeType,
            w.vowType,
            w.chrType,
            w.teamType,
            w.playerParamChrType,
            w.hasHp ? w.hp : -1,
            w.hasHp ? w.hpMax : -1,
            w.hpCurrentOffset,
            w.hpMaxOffset,
            RoleTextFromWorldRow(w),
            w.inGameName.empty() ? "" : w.inGameName.c_str()
        );

        if (w.valid && LooksLikePointer(w.phantomData))
        {
            char ph0[384];
            char ph80[384];
            HexBytesForDump(w.phantomData, 64, ph0, sizeof(ph0));
            HexBytesForDump(w.phantomData + 0x80, 64, ph80, sizeof(ph80));
            WriteLogf("PHANTOM_BYTES[%s] base=%08X +00=%s", w.slotName, (DWORD)w.phantomData, ph0);
            WriteLogf("PHANTOM_BYTES[%s] base=%08X +80=%s", w.slotName, (DWORD)w.phantomData, ph80);
        }

        if (w.valid && LooksLikePointer(w.playerParam))
        {
            char pp0[384];
            char pp80[384];
            HexBytesForDump(w.playerParam, 64, pp0, sizeof(pp0));
            HexBytesForDump(w.playerParam + 0x80, 64, pp80, sizeof(pp80));
            WriteLogf("PPARAM_BYTES[%s] base=%08X +00=%s", w.slotName, (DWORD)w.playerParam, pp0);
            WriteLogf("PPARAM_BYTES[%s] base=%08X +80=%s", w.slotName, (DWORD)w.playerParam, pp80);
        }
    }

    for (int pno = 0; pno < MAX_TRACKED_PLAYER_NO; pno++)
    {
        PlayerIdentity id;
        if (SnapshotIdentityForPlayerNo(pno, id))
        {
            WriteLogf(
                "IDENTITY[pno=%d] steam=%08X%08X age=%lu count=%ld",
                pno,
                (DWORD)(id.steamId >> 32),
                (DWORD)(id.steamId & 0xFFFFFFFF),
                (DWORD)(GetTickCount64() - id.lastSeenTick),
                id.learnCount
            );
        }
    }

    for (std::unordered_map<uint64_t, SteamNodeInfo>::const_iterator it = nodesById.begin(); it != nodesById.end(); ++it)
    {
        const SteamNodeInfo& n = it->second;
        WriteLogf(
            "STEAM steam=%08X%08X local=%d ping=%d st=%d cid=%d name=%s text=%s",
            (DWORD)(n.steamId >> 32),
            (DWORD)(n.steamId & 0xFFFFFFFF),
            n.isLocal ? 1 : 0,
            n.hasPing ? n.ping.ping : -1,
            n.hasPing ? n.ping.status : -1,
            n.hasPing ? n.ping.cid : -1,
            n.name.c_str(),
            n.steamIdText.c_str()
        );
    }

    for (size_t i = 0; i < overlayRows.size(); i++)
    {
        const OverlayRow& r = overlayRows[i];
        WriteLogf(
            "ROW[%d] slot=%s pno=%d steam=%08X%08X name=%s worldName=%s hasName=%d ping=%d truePing=%d rtt=%d hp=%d/%d hpOff=%d/%d st=%d cid=%d phType=%d age=%lu",
            (int)i,
            r.slotName,
            r.playerNo,
            (DWORD)(r.steamId >> 32),
            (DWORD)(r.steamId & 0xFFFFFFFF),
            r.name.c_str(),
            r.worldName.c_str(),
            r.hasName ? 1 : 0,
            r.hasPing ? r.ping : -1,
            r.hasTruePing ? r.truePing : -1,
            r.hasTruePing ? r.trueRtt : -1,
            r.hasHp ? r.hp : -1,
            r.hasHp ? r.hpMax : -1,
            r.hpCurrentOffset,
            r.hpMaxOffset,
            r.status,
            r.cid,
            r.phantomType,
            (DWORD)r.learnedAgeMs
        );
    }
}

static void HexBytesLimited(const BYTE* data, uint32_t size, char* out, int outSize)
{
    if (!out || outSize <= 0)
        return;

    out[0] = 0;
    if (!data)
        return;

    static const char* hex = "0123456789ABCDEF";
    uint32_t maxBytes = size;
    if (maxBytes > 64)
        maxBytes = 64;

    int pos = 0;
    for (uint32_t i = 0; i < maxBytes && pos + 4 < outSize; ++i)
    {
        BYTE b = data[i];
        out[pos++] = hex[(b >> 4) & 0xF];
        out[pos++] = hex[b & 0xF];
        if (i + 1 < maxBytes)
            out[pos++] = ' ';
    }

    out[pos] = 0;
}

static int FindDwordInBytes(const BYTE* data, uint32_t size, uint32_t value)
{
    if (!data || size < 4)
        return -1;

    for (uint32_t i = 0; i + 4 <= size; ++i)
    {
        uint32_t v = 0;
        memcpy(&v, data + i, sizeof(v));
        if (v == value)
            return (int)i;
    }

    return -1;
}

static bool TryDecodePlayerNoFromP2PPacket(const BYTE* packetBuf, uint32_t bytesRead, int* outPlayerNo, int* outType14Offset)
{
    if (outPlayerNo)
        *outPlayerNo = -1;

    if (outType14Offset)
        *outType14Offset = -1;

    if (!packetBuf || bytesRead < 30)
        return false;

    uint16_t hdr = 0;
    uint32_t family = 0;
    uint32_t eventType = 0;
    memcpy(&hdr, packetBuf + 0, sizeof(hdr));
    memcpy(&family, packetBuf + 2, sizeof(family));
    memcpy(&eventType, packetBuf + 6, sizeof(eventType));

    if (outType14Offset && eventType == 0x14u)
        *outType14Offset = 6;

    // The reliable identity bridge packet observed in the logs is:
    //   72 17 | 0C 00 00 00 | 14 00 00 00 | ... | senderPlayerNo
    // v17 was too loose and learned from family 0x15 / 0x27 packets, which produced false pno=4 churn.
    if (hdr != 0x1772 || family != 0x0Cu || eventType != 0x14u)
        return false;

    uint32_t tail = 0;
    memcpy(&tail, packetBuf + bytesRead - 4, sizeof(tail));
    if (tail >= 1 && tail < (uint32_t)MAX_TRACKED_PLAYER_NO)
    {
        if (outPlayerNo)
            *outPlayerNo = (int)tail;
        return true;
    }

    // Conservative fallback for same packet family only: pick the last valid dword after event type.
    int found = -1;
    for (uint32_t off = 10; off + 4 <= bytesRead; off += 4)
    {
        uint32_t v = 0;
        memcpy(&v, packetBuf + off, sizeof(v));
        if (v >= 1 && v < (uint32_t)MAX_TRACKED_PLAYER_NO)
            found = (int)v;
    }

    if (found >= 0)
    {
        if (outPlayerNo)
            *outPlayerNo = found;
        return true;
    }

    return false;
}

static void LearnIdentityFromP2P(int playerNo, uint64_t steamId)
{
    if (!IsValidPlayerNo(playerNo) || steamId == 0)
        return;

    if (!g_identityLockReady)
        return;

    ULONGLONG now = GetTickCount64();

    EnterCriticalSection(&g_identityLock);

    PlayerIdentity& id = g_identityByPlayerNo[playerNo];
    bool changed = (id.steamId != steamId);

    id.playerNo = playerNo;
    id.steamId = steamId;
    id.lastSeenTick = now;
    id.learnCount++;

    LeaveCriticalSection(&g_identityLock);

    if (changed || g_debugP2PBridge)
    {
        WriteLogf(
            "P2P_ID_LEARN pno=%d steam=%08X%08X changed=%d",
            playerNo,
            (DWORD)(steamId >> 32),
            (DWORD)(steamId & 0xFFFFFFFF),
            changed ? 1 : 0
        );
    }
}

static void RememberRecentP2PRemoteSteamId(uint64_t steamId)
{
    if (steamId == 0 || !g_identityLockReady)
        return;
    if (g_localSteamId != 0 && steamId == g_localSteamId)
        return;

    EnterCriticalSection(&g_identityLock);
    if (g_localSteamId == 0 || steamId != g_localSteamId)
        g_recentP2PRemoteSteamIds[steamId] = GetTickCount64();
    LeaveCriticalSection(&g_identityLock);
}

extern "C" __declspec(noinline) void __cdecl OnSteamP2PReadSuccess(
    uintptr_t steamSessionLight,
    const BYTE* packetBuf,
    uint32_t bytesRead,
    uint32_t steamIdLow,
    uint32_t steamIdHigh
)
{
    if (!packetBuf || bytesRead < 2)
        return;

    uint64_t steamId = ((uint64_t)steamIdHigh << 32) | (uint64_t)steamIdLow;
    RememberRecentP2PRemoteSteamId(steamId);

    uint16_t packetHeader = 0;
    memcpy(&packetHeader, packetBuf, sizeof(packetHeader));

    int playerNo = -1;
    int type14Offset = -1;
    bool decoded = TryDecodePlayerNoFromP2PPacket(packetBuf, bytesRead, &playerNo, &type14Offset);

    if (decoded)
        LearnIdentityFromP2P(playerNo, steamId);

    if (g_debugP2PBridge)
    {
        LONG seq = InterlockedIncrement(&g_p2pRxSeq);
        char hexLine[256];
        HexBytesLimited(packetBuf, bytesRead, hexLine, sizeof(hexLine));

        if (seq <= 800 || decoded || type14Offset >= 0 || packetHeader == 0x2714)
        {
            WriteLogf(
                "P2P_RX seq=%ld ssl=%08X steam=%08X%08X bytes=%u hdr=%04X type14Off=%d decodedPno=%d hex=%s",
                seq,
                (DWORD)steamSessionLight,
                steamIdHigh,
                steamIdLow,
                bytesRead,
                packetHeader,
                type14Offset,
                decoded ? playerNo : -1,
                hexLine
            );
        }
    }
}


extern "C" __declspec(noinline) void __cdecl OnHpWriteHook(uintptr_t chr, int writtenHp, DWORD siteVa)
{
    // Dead simple rule: if the HP setter writes 1, force only the HP field green for the linger window.
    if (writtenHp != 1)
        return;

    if (!LooksLikePointer(chr))
        return;

    int hpMax = -1;
    if (!SafeRead(chr + (uintptr_t)DEFAULT_HP_MAX_OFFSET, hpMax))
        return;

    if (hpMax < 1 || hpMax > 99999)
        return;

    ULONGLONG until = GetTickCount64() + g_hpOneLingerMs;

    if (g_rowsLockReady)
    {
        EnterCriticalSection(&g_rowsLock);
        g_oneHpUntilByChr[chr] = until;

        for (size_t i = 0; i < g_rows.size(); i++)
        {
            if (g_rows[i].chr == chr)
            {
                g_rows[i].hpOneUntilTick = until;
                g_rows[i].hasHp = true;
                g_rows[i].hp = 1;
                if (g_rows[i].hpMax < 1)
                    g_rows[i].hpMax = hpMax;
            }
        }

        LeaveCriticalSection(&g_rowsLock);
    }

    if (g_debugP2PBridge)
        WriteLogf("HP_WRITE_ONE site=%08X chr=%08X hp=1 max=%d lingerMs=%lu", siteVa, (DWORD)chr, hpMax, g_hpOneLingerMs);
}

#ifdef _M_IX86
extern "C" __declspec(naked) void Hook_SteamP2PReadSuccess()
{
    __asm
    {
        // Hooked at VA 0072BBBA, after ReadP2PPacket succeeded.
        // At this point, original function has:
        //   EBX = SteamSessionLight* this
        //   EBP = packet buffer pointer
        //   [ESP+0x14] = bytesRead / message size from Steam
        //   [ESP+0x18] = remote CSteamID low32
        //   [ESP+0x1C] = remote CSteamID high32
        pushfd
        pushad

        mov eax, [esp + 0x40] // steamIdHigh: original [esp+0x1C]
        mov ebx, [esp + 0x3C] // steamIdLow:  original [esp+0x18]
        mov edx, [esp + 0x38] // bytesRead:   original [esp+0x14]
        mov ecx, [esp + 0x08] // packetBuf:   saved original EBP
        mov esi, [esp + 0x10] // this:        saved original EBX

        push eax
        push ebx
        push edx
        push ecx
        push esi
        call OnSteamP2PReadSuccess
        add esp, 20

        popad
        popfd
        jmp dword ptr[g_steamP2PReadSuccessTrampoline]
    }
}

extern "C" __declspec(naked) void Hook_HpWrite_EAX_00E6891D()
{
    __asm
    {
        pushfd
        pushad

        mov ecx, [esp + 0x08] // original EBP = chr
        mov eax, [esp + 0x1C] // original EAX = written HP

        push 0x00E6891D
        push eax
        push ecx
        call OnHpWriteHook
        add esp, 12

        popad
        popfd
        jmp dword ptr[g_hpWriteEax_00E6891D_Trampoline]
    }
}

extern "C" __declspec(naked) void Hook_HpWrite_EBX_00E68960()
{
    __asm
    {
        pushfd
        pushad

        mov ecx, [esp + 0x08] // original EBP = chr
        mov eax, [esp + 0x10] // original EBX = written HP

        push 0x00E68960
        push eax
        push ecx
        call OnHpWriteHook
        add esp, 12

        popad
        popfd
        jmp dword ptr[g_hpWriteEbx_00E68960_Trampoline]
    }
}

extern "C" __declspec(naked) void Hook_HpWrite_EBX_00E68981()
{
    __asm
    {
        pushfd
        pushad

        mov ecx, [esp + 0x08] // original EBP = chr
        mov eax, [esp + 0x10] // original EBX = written HP

        push 0x00E68981
        push eax
        push ecx
        call OnHpWriteHook
        add esp, 12

        popad
        popfd
        jmp dword ptr[g_hpWriteEbx_00E68981_Trampoline]
    }
}

extern "C" __declspec(naked) void Hook_HpWrite_EBX_00E68991()
{
    __asm
    {
        pushfd
        pushad

        mov ecx, [esp + 0x08] // original EBP = chr
        mov eax, [esp + 0x10] // original EBX = written HP

        push 0x00E68991
        push eax
        push ecx
        call OnHpWriteHook
        add esp, 12

        popad
        popfd
        jmp dword ptr[g_hpWriteEbx_00E68991_Trampoline]
    }
}
#endif

static bool WriteRel32Jump(void* src, void* dst, int len, const char* name)
{
    if (len < 5)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(src, len, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        WriteLogf("Hook %s VirtualProtect failed err=%lu", name, GetLastError());
        return false;
    }

    BYTE* p = (BYTE*)src;
    intptr_t rel = (intptr_t)dst - ((intptr_t)src + 5);

    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)rel;

    for (int i = 5; i < len; i++)
        p[i] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(src, len, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), src, len);

    WriteLogf("Hook %s JMP installed at %08X -> %08X len=%d", name, (DWORD)(uintptr_t)src, (DWORD)(uintptr_t)dst, len);
    return true;
}

static bool InstallInlineHook(void* target, void* detour, int len, void** trampoline, const char* name)
{
    BYTE* gateway = (BYTE*)VirtualAlloc(NULL, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!gateway)
    {
        WriteLogf("Hook %s trampoline alloc failed err=%lu", name, GetLastError());
        return false;
    }

    CopyMemory(gateway, target, len);
    gateway[len] = 0xE9;
    *(int32_t*)(gateway + len + 1) = (int32_t)((intptr_t)target + len - ((intptr_t)gateway + len + 5));

    *trampoline = gateway;

    if (!WriteRel32Jump(target, detour, len, name))
        return false;

    WriteLogf("Hook %s trampoline=%08X", name, (DWORD)(uintptr_t)gateway);
    return true;
}


static void LogBytes8(const char* name, uintptr_t address)
{
    BYTE* b = (BYTE*)address;
    WriteLogf(
        "%s bytes at %08X: %02X %02X %02X %02X %02X %02X %02X %02X",
        name,
        (DWORD)address,
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]
    );
}

static bool PatchBytes(void* dst, const BYTE* src, int len, const char* name)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        WriteLogf("Patch %s VirtualProtect failed at %08X err=%lu", name, (DWORD)(uintptr_t)dst, GetLastError());
        return false;
    }

    CopyMemory(dst, src, len);

    DWORD ignored = 0;
    VirtualProtect(dst, len, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    WriteLogf("Patch %s wrote %d bytes at %08X", name, len, (DWORD)(uintptr_t)dst);
    return true;
}

#ifdef _M_IX86
extern "C" __declspec(naked) void Stub_ReturnHomePopup_Ret8()
{
    __asm
    {
        ret 8
    }
}
#endif

static bool PatchRel32Call(void* callSite, void* dst, const char* name)
{
    BYTE* p = (BYTE*)callSite;
    if (p[0] != 0xE8)
    {
        WriteLogf("Patch %s refused: %08X is not CALL rel32, byte=%02X", name, (DWORD)(uintptr_t)callSite, p[0]);
        return false;
    }

    BYTE patch[5];
    patch[0] = 0xE8;
    *(int32_t*)(patch + 1) = (int32_t)((intptr_t)dst - ((intptr_t)callSite + 5));
    return PatchBytes(callSite, patch, 5, name);
}

static bool NopBytes(void* dst, int len, const char* name)
{
    BYTE patch[16];
    if (len <= 0 || len > (int)sizeof(patch))
        return false;

    for (int i = 0; i < len; i++)
        patch[i] = 0x90;

    return PatchBytes(dst, patch, len, name);
}


static bool ShouldSuppressRequestPopupMessageId(int msgId)
{
    // These are the dismissable return/send-home request-menu popups, not the normal overlay roster.
    // Confirmed/nearby FMG strings:
    //   150013   <?leaveName?> has returned home
    //   20000420 Sent home by summoner. Returning to your world.
    //   20000421 Sent home by summoner. Returning to your world.
    //   20000425 Phantom <?leaveName?> has returned home
    //   20000430 You returned home
    //   20000435 Phantom <?leaveName?> has returned home
    //   20000450 You were sent home
    //   20001020 Phantom <?leaveName?> has returned home
    switch (msgId)
    {
    case 150013:
    case 20000420:
    case 20000421:
    case 20000425:
    case 20000430:
    case 20000435:
    case 20000450:
    case 20001020:
        return true;
    default:
        return false;
    }
}

#ifdef _M_IX86
typedef void(__stdcall* RecallMenuEvent_00D74950_Fn)(int category, int msgId);

static void __stdcall Hook_RecallMenuEvent_00D74950(int category, int msgId)
{
    if (ShouldSuppressRequestPopupMessageId(msgId))
    {
        WriteLogf("Suppress RecallMenuEvent popup category=%d msgId=%d", category, msgId);
        return;
    }

    ((RecallMenuEvent_00D74950_Fn)g_recallMenuEvent_00D74950_Trampoline)(category, msgId);
}
#endif

static bool InstallRecallMenuEventPopupFilter()
{
#ifdef _M_IX86
    uintptr_t target = Rva(OFF_RECALL_MENU_EVENT_00D74950);
    BYTE* b = (BYTE*)target;

    WriteLogf(
        "RecallMenuEvent_00D74950 bytes at %08X: %02X %02X %02X %02X %02X %02X %02X %02X",
        (DWORD)target,
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]
    );

    // Actual PTDE prologue from Ghidra:
    //   55                   push ebp
    //   8B EC                mov  ebp, esp
    //   83 E4 F8             and  esp, FFFFFFF8h
    // The function ends in RET 08, so the detour is __stdcall.
    if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC && b[3] == 0x83 && b[4] == 0xE4 && b[5] == 0xF8)
        return InstallInlineHook((void*)target, (void*)Hook_RecallMenuEvent_00D74950, 6, &g_recallMenuEvent_00D74950_Trampoline, "RecallMenuEvent_00D74950_filter_return_home_popups_stdcall");

    // Fallback: do not guess. Paste these bytes if this refuses.
    WriteLogLine("RecallMenuEvent_00D74950 filter not installed: unexpected prologue bytes; paste RecallMenuEvent bytes above.");
#else
    WriteLogLine("RecallMenuEvent_00D74950 filter not installed: this code requires 32-bit _M_IX86 build.");
#endif
    return false;
}


#ifdef _M_IX86
// FUN_00d4c190 calling convention from Ghidra/callsite:
//   ECX = message id
//   [ESP+4] = popup mode/type, commonly 0x1E
//   RET, caller cleans stack with ADD ESP,4
//
// This naked hook returns directly for the filtered messages, leaving the caller's stack
// exactly as the original plain RET would. Do NOT use __stdcall here.
extern "C" __declspec(naked) void Hook_RequestPopup_00D4C190_FilterReturnHome()
{
    __asm
    {
        // Only suppress the request-popup form used by the return-home path.
        cmp dword ptr[esp + 4], 01Eh
        jne allow_original

        // 150013 = 000249FDh
        cmp ecx, 000249FDh
        je suppress

        // 20000420.. etc. These are already hex values, not RVAs.
        cmp ecx, 01312EA4h
        je suppress
        cmp ecx, 01312EA5h
        je suppress
        cmp ecx, 01312EA9h
        je suppress
        cmp ecx, 01312EAEh
        je suppress
        cmp ecx, 01312EB3h
        je suppress
        cmp ecx, 01312EC2h
        je suppress
        cmp ecx, 013130FCh
        je suppress

        allow_original :
        jmp dword ptr[g_requestPopup_00D4C190_Trampoline]

            suppress :
            // Original function uses plain RET. Caller will still ADD ESP,4.
            ret
    }
}
#endif

static bool InstallRequestPopup_00D4C190_Filter()
{
#ifdef _M_IX86
    uintptr_t target = Rva(OFF_REQUEST_POPUP_00D4C190);
    BYTE* b = (BYTE*)target;

    WriteLogf(
        "RequestPopup_00D4C190 bytes at %08X: %02X %02X %02X %02X %02X %02X %02X %02X",
        (DWORD)target,
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]
    );

    // Actual PTDE prologue from Ghidra:
    //   55                   push ebp
    //   8B EC                mov  ebp, esp
    //   83 E4 F8             and  esp, FFFFFFF8h
    if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC && b[3] == 0x83 && b[4] == 0xE4 && b[5] == 0xF8)
        return InstallInlineHook((void*)target, (void*)Hook_RequestPopup_00D4C190_FilterReturnHome, 6, &g_requestPopup_00D4C190_Trampoline, "RequestPopup_00D4C190_filter_return_home_popups_ecx_msgid");

    WriteLogLine("RequestPopup_00D4C190 filter not installed: unexpected prologue bytes; paste RequestPopup bytes above.");
#else
    WriteLogLine("RequestPopup_00D4C190 filter not installed: this code requires 32-bit _M_IX86 build.");
#endif
    return false;
}

static bool InstallReturnedHomePopupSuppressorForCaller(uintptr_t callerRva, int scanLen, const char* label)
{
#ifdef _M_IX86
    uintptr_t caller = Rva(callerRva);
    uintptr_t display = Rva(OFF_RECALL_MENU_EVENT_00D74950);
    BYTE* bytes = (BYTE*)caller;

    WriteLogf(
        "%s suppressor scan caller=%08X display=%08X scanLen=%X",
        label,
        (DWORD)caller,
        (DWORD)display,
        scanLen
    );
    LogBytes8(label, caller);

    bool anyOk = false;
    for (int i = 0; i < scanLen - 5; i++)
    {
        if (bytes[i] != 0xE8)
            continue;

        uintptr_t callSite = caller + i;
        int32_t rel = *(int32_t*)(bytes + i + 1);
        uintptr_t target = callSite + 5 + rel;

        if (target != display)
            continue;

        WriteLogf("%s display CALL found at %08X -> %08X", label, (DWORD)callSite, (DWORD)target);
        LogBytes8("ReturnedHome callsite before", callSite);

        // FUN_00d74950 ends with RET 08. Replacing the call with a tiny RET 08-compatible stub is
        // safer than NOPing, because most callers expect the callee to clean category+msgId.
        char patchName[160];
        wsprintfA(patchName, "%s_CALL_ret8_stub_to_00D74950", label);
        bool ok = PatchRel32Call((void*)callSite, (void*)Stub_ReturnHomePopup_Ret8, patchName);
        LogBytes8("ReturnedHome callsite after", callSite);
        anyOk = ok || anyOk;
    }

    if (!anyOk)
        WriteLogf("%s suppressor not installed: no CALL to FUN_00d74950 found in scan window.", label);
    return anyOk;
#else
    WriteLogf("%s suppressor not installed: this code requires 32-bit _M_IX86 build.", label);
    return false;
#endif
}

static bool InstallReturnedHomePopupSuppressor()
{
#ifdef _M_IX86
    bool ok = false;
    ok = InstallReturnedHomePopupSuppressorForCaller(OFF_RETURNED_HOME_POPUP_CALLER_00EA1BF0, RETURNED_HOME_CALLER_SCAN_LEN_00EA1BF0, "ReturnedHome_00EA1BF0") || ok;
    ok = InstallReturnedHomePopupSuppressorForCaller(OFF_RETURNED_HOME_POPUP_CALLER_00EA2510, RETURNED_HOME_CALLER_SCAN_LEN_00EA2510, "ReturnedHome_00EA2510") || ok;
    return ok;
#else
    WriteLogLine("ReturnedHome suppressor not installed: this code requires 32-bit _M_IX86 build.");
    return false;
#endif
}

static bool InstallHpWriteHookIfBytesMatch(uintptr_t rva, void* detour, void** trampoline, const char* name, BYTE regOpcode)
{
#ifdef _M_IX86
    uintptr_t target = Rva(rva);
    BYTE* b = (BYTE*)target;

    WriteLogf(
        "%s bytes at %08X: %02X %02X %02X %02X %02X %02X %02X %02X",
        name,
        (DWORD)target,
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]
    );

    // Expected:
    //   EAX: 89 85 D4 02 00 00    mov [ebp+000002D4], eax
    //   EBX: 89 9D D4 02 00 00    mov [ebp+000002D4], ebx
    if (b[0] == 0x89 && b[1] == regOpcode && b[2] == 0xD4 && b[3] == 0x02 && b[4] == 0x00 && b[5] == 0x00)
        return InstallInlineHook((void*)target, detour, 6, trampoline, name);

    WriteLogf("%s not installed: unexpected bytes; paste bytes above.", name);
#else
    (void)rva; (void)detour; (void)trampoline; (void)name; (void)regOpcode;
#endif
    return false;
}

static void InstallHooks()
{
#ifdef _M_IX86
    if (g_hooksInstalled)
        return;

    bool anyOk = false;

    uintptr_t p2pSuccess = Rva(OFF_STEAM_P2P_READ_SUCCESS);
    BYTE* p2pBytes = (BYTE*)p2pSuccess;

    WriteLogf(
        "SteamP2PReadSuccess bytes at %08X: %02X %02X %02X %02X %02X %02X %02X %02X",
        (DWORD)p2pSuccess,
        p2pBytes[0], p2pBytes[1], p2pBytes[2], p2pBytes[3],
        p2pBytes[4], p2pBytes[5], p2pBytes[6], p2pBytes[7]
    );

    if (p2pBytes[0] == 0x8B && p2pBytes[1] == 0x7C && p2pBytes[2] == 0x24 && p2pBytes[3] == 0x14 &&
        p2pBytes[4] == 0x83 && p2pBytes[5] == 0xFF && p2pBytes[6] == 0x02)
    {
        if (InstallInlineHook((void*)p2pSuccess, (void*)Hook_SteamP2PReadSuccess, 7, &g_steamP2PReadSuccessTrampoline, "SteamP2PReadSuccess_0072BBBA"))
            anyOk = true;
    }
    else
    {
        WriteLogLine("SteamP2PReadSuccess_0072BBBA not installed: unexpected bytes; paste SteamP2PReadSuccess bytes.");
    }

    bool returnedHomePopupOk = InstallReturnedHomePopupSuppressor();
    if (returnedHomePopupOk)
        anyOk = true;

    bool recallMenuFilterOk = InstallRecallMenuEventPopupFilter();
    if (recallMenuFilterOk)
        anyOk = true;

    bool requestPopupFilterOk = InstallRequestPopup_00D4C190_Filter();
    if (requestPopupFilterOk)
        anyOk = true;

    bool hpOk = false;
    hpOk = InstallHpWriteHookIfBytesMatch(OFF_HP_WRITE_EAX_00E6891D, (void*)Hook_HpWrite_EAX_00E6891D, &g_hpWriteEax_00E6891D_Trampoline, "HpWrite_EAX_00E6891D", 0x85) || hpOk;
    hpOk = InstallHpWriteHookIfBytesMatch(OFF_HP_WRITE_EBX_00E68960, (void*)Hook_HpWrite_EBX_00E68960, &g_hpWriteEbx_00E68960_Trampoline, "HpWrite_EBX_00E68960", 0x9D) || hpOk;
    hpOk = InstallHpWriteHookIfBytesMatch(OFF_HP_WRITE_EBX_00E68981, (void*)Hook_HpWrite_EBX_00E68981, &g_hpWriteEbx_00E68981_Trampoline, "HpWrite_EBX_00E68981", 0x9D) || hpOk;
    hpOk = InstallHpWriteHookIfBytesMatch(OFF_HP_WRITE_EBX_00E68991, (void*)Hook_HpWrite_EBX_00E68991, &g_hpWriteEbx_00E68991_Trampoline, "HpWrite_EBX_00E68991", 0x9D) || hpOk;

    if (hpOk)
        anyOk = true;

    g_hooksInstalled = anyOk;
    WriteLogf("Hooks installed: any=%d returnedHomePopup=%d recallMenuFilter=%d requestPopupFilter=%d hpWrite=%d", anyOk ? 1 : 0, returnedHomePopupOk ? 1 : 0, recallMenuFilterOk ? 1 : 0, requestPopupFilterOk ? 1 : 0, hpOk ? 1 : 0);
#else
    WriteLogLine("Hooks not installed: this code requires 32-bit _M_IX86 build.");
#endif
}

static void MakeOverlayOwnedByGame()
{
    if (g_overlayWnd == NULL)
        return;

    if (g_gameWnd == NULL || !IsWindow(g_gameWnd))
        return;

    if (g_overlayOwnerWnd == g_gameWnd)
        return;

    SetWindowLongPtrA(g_overlayWnd, GWLP_HWNDPARENT, (LONG_PTR)g_gameWnd);
    g_overlayOwnerWnd = g_gameWnd;
    WriteLogf("Overlay owner set to game window: %08X", (DWORD)g_gameWnd);
}

static void UpdateOverlayPosition()
{
    if (g_overlayWnd == NULL)
        return;

    HWND found = FindGameWindow();

    if (found == NULL || found == g_overlayWnd || !IsWindow(found) || !IsWindowVisible(found) || IsIconic(found))
    {
        g_gameWnd = NULL;
        g_overlayOwnerWnd = NULL;
        ShowWindow(g_overlayWnd, SW_HIDE);
        return;
    }

    g_gameWnd = found;

    RECT client;
    if (!GetClientRect(g_gameWnd, &client))
    {
        ShowWindow(g_overlayWnd, SW_HIDE);
        return;
    }

    POINT topLeft;
    topLeft.x = 0;
    topLeft.y = 0;
    ClientToScreen(g_gameWnd, &topLeft);

    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        ShowWindow(g_overlayWnd, SW_HIDE);
        return;
    }

    // Apply the window styles + colour-key once (they never change at runtime). Doing
    // this every timer tick forced needless window-manager work and recomposition.
    if (!g_overlayStyleApplied)
    {
        DWORD exStyle = GetWindowLongA(g_overlayWnd, GWL_EXSTYLE);
        exStyle |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (g_forceTopmost) exStyle |= WS_EX_TOPMOST; else exStyle &= ~WS_EX_TOPMOST;
        SetWindowLongA(g_overlayWnd, GWL_EXSTYLE, exStyle);
        SetLayeredWindowAttributes(g_overlayWnd, OVERLAY_TRANSPARENT_KEY, 255, LWA_COLORKEY);
        MakeOverlayOwnedByGame();
        g_overlayStyleApplied = true;
    }

    // Only move/resize when the target actually changed — and never with SWP_FRAMECHANGED,
    // which forced a non-client recalc + recomposition on every tick.
    if (topLeft.x != g_lastOverlayRect.left || topLeft.y != g_lastOverlayRect.top ||
        width != g_lastOverlayRect.right || height != g_lastOverlayRect.bottom)
    {
        SetWindowPos(
            g_overlayWnd,
            g_forceTopmost ? HWND_TOPMOST : HWND_TOP,
            topLeft.x, topLeft.y, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW
        );
        g_lastOverlayRect.left = topLeft.x;
        g_lastOverlayRect.top = topLeft.y;
        g_lastOverlayRect.right = width;
        g_lastOverlayRect.bottom = height;
    }
}

static void DrawTextShadow(HDC hdc, int x, int y, COLORREF color, const char* text)
{
    SetTextColor(hdc, RGB(1, 1, 1));
    TextOutA(hdc, x + 1, y + 1, text, lstrlenA(text));

    SetTextColor(hdc, color);
    TextOutA(hdc, x, y, text, lstrlenA(text));
}

static void DrawTextOutlineThick(HDC hdc, int x, int y, COLORREF outline, COLORREF fill, const char* text)
{
    const int len = lstrlenA(text);

    // Thicker readable border for host and non-invader phantoms.
    // Two-pixel full outline: cardinal + diagonal offsets at radius 1 and 2.
    SetTextColor(hdc, outline);

    for (int oy = -2; oy <= 2; oy++)
    {
        for (int ox = -2; ox <= 2; ox++)
        {
            if (ox == 0 && oy == 0)
                continue;

            if (ox * ox + oy * oy > 5)
                continue;

            TextOutA(hdc, x + ox, y + oy, text, len);
        }
    }

    SetTextColor(hdc, fill);
    TextOutA(hdc, x, y, text, len);
}

static void DrawTextOutlineInvaderBlackWhite(HDC hdc, int x, int y, COLORREF outline, COLORREF fill, const char* text)
{
    const int len = lstrlenA(text);

    // Invader: maximum visibility; solid black fill with thick red border.
    // The overlay transparent key is bright magenta, so pure black is safe.
    SetTextColor(hdc, outline);

    for (int oy = -3; oy <= 3; oy++)
    {
        for (int ox = -3; ox <= 3; ox++)
        {
            if (ox == 0 && oy == 0)
                continue;

            if (ox * ox + oy * oy > 10)
                continue;

            TextOutA(hdc, x + ox, y + oy, text, len);
        }
    }

    SetTextColor(hdc, fill);

    // Draw a heavy center without expanding the row width too much.
    TextOutA(hdc, x, y, text, len);
    TextOutA(hdc, x + 1, y, text, len);
    TextOutA(hdc, x, y + 1, text, len);
    TextOutA(hdc, x + 1, y + 1, text, len);
}

static void DrawTextOutlineHpOneBold(HDC hdc, int x, int y, const char* text)
{
    const int len = lstrlenA(text);

    // Competition marker: 1 HP rows become bold near-black with green border.
    // v36 uses a bright magenta overlay colorkey, so pure RGB(0,0,0) is safe and opaque.
    SetTextColor(hdc, RGB(40, 255, 80));
    TextOutA(hdc, x - 1, y, text, len);
    TextOutA(hdc, x + 1, y, text, len);
    TextOutA(hdc, x, y - 1, text, len);
    TextOutA(hdc, x, y + 1, text, len);
    TextOutA(hdc, x - 1, y - 1, text, len);
    TextOutA(hdc, x + 1, y - 1, text, len);
    TextOutA(hdc, x - 1, y + 1, text, len);
    TextOutA(hdc, x + 1, y + 1, text, len);

    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, x, y, text, len);
    TextOutA(hdc, x + 1, y, text, len);
    TextOutA(hdc, x, y + 1, text, len);
    TextOutA(hdc, x + 1, y + 1, text, len);
}

static void DrawOverlayName(HDC hdc, int x, int y, const OverlayRow& row, const char* text)
{
    if (row.disconnected)
    {
        // Disconnected placeholder row: muted gray text, black outline.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(150, 150, 150), text);
        return;
    }

    if (LiveChrTeamStyleKindForRow(row) == STICKY_ROUTE_HOST_LIVE_TYPE)
    {
        // Live ChrType/TeamType was changed to human/hollow host appearance.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(255, 165, 60), text);
        return;
    }

    if (IsSunlightSummonWorldRow(row))
    {
        // Warrior of Sunlight white summon: yellow text with black border.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(255, 220, 40), text);
        return;
    }

    if (IsWhiteSummonWorldRow(row))
    {
        // Normal white summon: white text with black border.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(245, 245, 245), text);
        return;
    }

    if (IsDragonSummonWorldRow(row))
    {
        // Path of the Dragon summon: tan text with black border.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(210, 180, 120), text);
        return;
    }

    if (IsForestHunterWorldRow(row))
    {
        // Cat Ring / Forest Hunter invader: black text with cyan border.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 220, 255), RGB(0, 0, 0), text);
        return;
    }

    if (IsGravelordBlueWorldRow(row))
    {
        // Gravelord Servant / Spirit of Vengeance route: black text with purple border.
        DrawTextOutlineThick(hdc, x, y, RGB(170, 70, 255), RGB(0, 0, 0), text);
        return;
    }

    if (IsDarkmoonBlueWorldRow(row))
    {
        // Darkmoon Spirit of Vengeance: black text with blue border.
        DrawTextOutlineThick(hdc, x, y, RGB(80, 150, 255), RGB(0, 0, 0), text);
        return;
    }

    if (IsGenericBlueAppearanceWorldRow(row))
    {
        // Conservative blue-route fallback: do not ever render blue phantoms as
        // blue text with a black border. If the exact blue route is not resolved
        // yet, use the Darkmoon-style black text with blue border until it is.
        DrawTextOutlineThick(hdc, x, y, RGB(80, 150, 255), RGB(0, 0, 0), text);
        return;
    }

    if (IsRedSummonWorldRow(row))
    {
        // Red-sign / red summon: red text with black border. Must be checked
        // before generic red invader styling.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(255, 70, 70), text);
        return;
    }

    if (IsInvaderWorldRow(row))
    {
        // Red-eye / hostile red invader: black text with red border.
        DrawTextOutlineInvaderBlackWhite(hdc, x, y, RGB(255, 40, 40), RGB(0, 0, 0), text);
        return;
    }

    if (row.isPhantom != 0)
    {
        // Normal red summons and uncertain phantoms: role color with black border.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), ColorForWorldRow(row), text);
        return;
    }

    // Host: orange text with black border.
    DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), ColorForWorldRow(row), text);
}


static void DrawOverlayHp(HDC hdc, int x, int y, const OverlayRow& row, const char* text)
{
    ULONGLONG now = GetTickCount64();
    if (row.hpOneUntilTick != 0 && now <= row.hpOneUntilTick)
    {
        // 1HP marker: only the HP field turns green. MS/name keep their normal role styling.
        DrawTextOutlineThick(hdc, x, y, RGB(0, 0, 0), RGB(40, 255, 80), text);
        return;
    }

    DrawOverlayName(hdc, x, y, row, text);
}

static void FormatFixedField(char* out, int outSize, int value, int width, const char* missing)
{
    if (!out || outSize <= 0)
        return;

    if (width < 1)
        width = 1;

    if (value < 0)
    {
        SafeLstrcpynA(out, missing, outSize);
        return;
    }

    char temp[32];
    wsprintfA(temp, "%d", value);

    int len = lstrlenA(temp);

    if (len > width)
    {
        // Keep the field fixed-width no matter what. Overflow becomes ###/#### instead of shifting the name.
        int n = min(width, outSize - 1);
        for (int i = 0; i < n; i++)
            out[i] = '#';
        out[n] = 0;
        return;
    }

    int spaces = width - len;
    int pos = 0;

    while (spaces > 0 && pos < outSize - 1)
    {
        out[pos++] = ' ';
        spaces--;
    }

    for (int i = 0; i < len && pos < outSize - 1; i++)
        out[pos++] = temp[i];

    out[pos] = 0;
}

// Returns true only when the INI's last-write time has changed since the last check
// (so we reload config on demand instead of opening/parsing the file every paint).
static bool IniChanged()
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fa))
        return false;
    if (!g_iniLastWriteValid || CompareFileTime(&fa.ftLastWriteTime, &g_iniLastWrite) != 0)
    {
        g_iniLastWrite = fa.ftLastWriteTime;
        g_iniLastWriteValid = true;
        return true;
    }
    return false;
}

// (Re)create the overlay fonts only when their parameters actually change, instead of
// calling CreateFontA/DeleteObject on every paint.
static void EnsureWindowFonts()
{
    if (g_winSmallFont && g_winHpFont &&
        g_winFontHeightCached == g_fontHeight &&
        g_winHpFontHeightCached == g_hpFontHeight &&
        lstrcmpA(g_winFontFaceCached, g_fontFace) == 0)
        return;

    if (g_winSmallFont) { DeleteObject(g_winSmallFont); g_winSmallFont = NULL; }
    if (g_winHpFont)    { DeleteObject(g_winHpFont);    g_winHpFont = NULL; }

    g_winSmallFont = CreateFontA(g_fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_fontFace);
    if (!g_winSmallFont)
        g_winSmallFont = CreateFontA(g_fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");

    g_winHpFont = CreateFontA(g_hpFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_fontFace);
    if (!g_winHpFont)
        g_winHpFont = CreateFontA(g_hpFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");

    g_winFontHeightCached = g_fontHeight;
    g_winHpFontHeightCached = g_hpFontHeight;
    lstrcpynA(g_winFontFaceCached, g_fontFace, sizeof(g_winFontFaceCached));
}

static void PaintOverlay(HWND hwnd)
{
    if (IniChanged())
        LoadConfig(true);
    EnsureWindowFonts();

    PAINTSTRUCT ps;
    HDC paintHdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int paintWidth = max(1, rc.right - rc.left);
    int paintHeight = max(1, rc.bottom - rc.top);

    HDC backHdc = CreateCompatibleDC(paintHdc);
    HBITMAP backBitmap = NULL;
    HBITMAP oldBitmap = NULL;
    HDC hdc = paintHdc;

    if (backHdc != NULL)
    {
        backBitmap = CreateCompatibleBitmap(paintHdc, paintWidth, paintHeight);
        if (backBitmap != NULL)
        {
            oldBitmap = (HBITMAP)SelectObject(backHdc, backBitmap);
            hdc = backHdc;
        }
    }

    HBRUSH transparentBrush = CreateSolidBrush(OVERLAY_TRANSPARENT_KEY);
    FillRect(hdc, &rc, transparentBrush);
    DeleteObject(transparentBrush);

    SetBkMode(hdc, TRANSPARENT);

    // Cached fonts (created once / on config change) — see EnsureWindowFonts().
    HFONT smallFont = g_winSmallFont;
    HFONT hpFont = g_winHpFont;

    HFONT oldFont = (HFONT)SelectObject(hdc, smallFont);

    TEXTMETRICA smallTm;
    ZeroMemory(&smallTm, sizeof(smallTm));
    GetTextMetricsA(hdc, &smallTm);

    SelectObject(hdc, hpFont);

    TEXTMETRICA hpTm;
    ZeroMemory(&hpTm, sizeof(hpTm));
    GetTextMetricsA(hdc, &hpTm);

    SIZE hpMaxSize;
    ZeroMemory(&hpMaxSize, sizeof(hpMaxSize));
    GetTextExtentPoint32A(hdc, "9999HP", 6, &hpMaxSize);

    SIZE markerMaxSize;
    ZeroMemory(&markerMaxSize, sizeof(markerMaxSize));
    // Wide enough for the largest prefix we draw: 999MS(C).
    // Local \[T]/ is centered inside this same field so it no longer hugs the left edge.
    SelectObject(hdc, smallFont);
    if (g_showPing)
        GetTextExtentPoint32A(hdc, "999MS(C)", 8, &markerMaxSize);

    if (g_showLocalMarker)
    {
        SIZE localMarkerMaxSize;
        ZeroMemory(&localMarkerMaxSize, sizeof(localMarkerMaxSize));
        GetTextExtentPoint32A(hdc, "\\[T]/", 5, &localMarkerMaxSize);
        if (localMarkerMaxSize.cx > markerMaxSize.cx)
            markerMaxSize.cx = localMarkerMaxSize.cx;
    }

    std::vector<OverlayRow> rows;

    if (g_rowsLockReady)
    {
        EnterCriticalSection(&g_rowsLock);
        rows = g_rows;
        LeaveCriticalSection(&g_rowsLock);
    }

    // v39 readable layout:
    // - no header
    // - no p1/p2/p3/p4 slots
    // - no role text
    // - no "no rows" filler
    // - format: ----HP [marker]      Name
    // - HP is drawn with a separate 2x font
    // - marker + name are vertically centered in the same row
    // - the marker is drawn near the HP field, not centered in a wide gutter
    // - the name x-position still never moves as HP/ping/marker changes
    int lineHeight = g_lineHeight;
    if (lineHeight < hpTm.tmHeight + 2)
        lineHeight = hpTm.tmHeight + 2;

    // v80: keep the v79 centered-gutter layout, but shrink the gutter.
    // v79 centered correctly but left too much space on both sides of the marker.
    // MarkerGutterExtra can be tuned in the INI without rebuilding.
    // HP column is gated by [HP] Enabled (subsystem) AND [Overlay] ShowHp (display).
    bool drawHpField = g_showHp && g_showHpField;
    // The marker gutter collapses to zero when neither marker can ever appear.
    bool anyMarker = g_showPing || g_showLocalMarker;
    int hpFieldWidth = drawHpField ? (hpMaxSize.cx + 6) : 0;
    int markerGutterWidth = anyMarker ? (markerMaxSize.cx + g_markerGutterExtra) : 0;
    int widthEstimate = hpFieldWidth + markerGutterWidth + 360;
    int totalLines = (int)rows.size();

    if (totalLines < 1)
    {
        SelectObject(hdc, oldFont); // fonts are cached; do not delete
        if (hdc == backHdc)
        {
            BitBlt(paintHdc, 0, 0, paintWidth, paintHeight, backHdc, 0, 0, SRCCOPY);
            if (oldBitmap)
                SelectObject(backHdc, oldBitmap);
            DeleteObject(backBitmap);
            DeleteDC(backHdc);
        }
        else if (backHdc)
        {
            DeleteDC(backHdc);
        }
        EndPaint(hwnd, &ps);
        return;
    }

    int blockHeight = totalLines * lineHeight;
    int overlayWidth = rc.right - rc.left;
    int overlayHeight = rc.bottom - rc.top;

    int x = g_paddingX;
    int y = g_paddingY;

    if (g_corner == CORNER_TOP_RIGHT || g_corner == CORNER_BOTTOM_RIGHT)
        x = overlayWidth - widthEstimate - g_paddingX;

    if (g_corner == CORNER_BOTTOM_LEFT || g_corner == CORNER_BOTTOM_RIGHT)
        y = overlayHeight - blockHeight - g_paddingY;

    if (x < g_paddingX)
        x = g_paddingX;

    if (y < g_paddingY)
        y = g_paddingY;

    // Master hotkey toggle: skip all row drawing but still blit the cleared buffer.
    for (size_t i = 0; g_overlayVisible && i < rows.size(); i++)
    {
        const OverlayRow& row = rows[i];

        char pingText[32];
        char pingSourceText[8];
        char hpText[32];
        char hpLine[64];
        char markerLine[64];
        const char* nameLine = row.name.c_str();

        if (g_showPing && row.hasPing && row.ping >= 0)
            FormatFixedField(pingText, sizeof(pingText), row.ping, 3, "---");
        else
            lstrcpyA(pingText, "---");

        pingSourceText[0] = 0;
        // v76 display rules:
        //   - true-ping rows no longer show a "(T)" suffix; numeric true ping is just NNNMS.
        //   - cached/session-table fallback rows may show "(C)".
        //   - missing true-ping is just ---MS, not ---MS(T).
        //   - the local/self row shows \[T]/ instead of a meaningless ---MS field.
        if (g_truePingShowSourceMarker && g_showPing && row.hasPing && row.ping >= 0 && !row.hasTruePing && !row.isLocal)
            lstrcpyA(pingSourceText, "(C)");

        if (g_showHp && row.hasHp && row.hp >= 0)
            FormatFixedField(hpText, sizeof(hpText), row.hp, 4, "----");
        else
            lstrcpyA(hpText, "----");

        wsprintfA(hpLine, "%sHP", hpText);
        if (row.isLocal)
            lstrcpyA(markerLine, "\\[T]/");
        else
            wsprintfA(markerLine, "%sMS%s", pingText, pingSourceText);

        if (row.disconnected)
        {
            // Placeholder for a player who left without dying: "Disconnected    ---MS    Name".
            lstrcpyA(hpLine, "Disconnected");
            lstrcpyA(markerLine, "---MS");
        }

        int hpY = y + ((lineHeight - hpTm.tmHeight) / 2);
        int smallY = y + ((lineHeight - smallTm.tmHeight) / 2);
        int markerGutterX = x + hpFieldWidth;
        int nameX = markerGutterX + markerGutterWidth;

        if (drawHpField)
        {
            SelectObject(hdc, hpFont);
            DrawOverlayHp(hdc, x, hpY, row, hpLine);
        }

        SelectObject(hdc, smallFont);

        bool drawMarker = row.isLocal ? g_showLocalMarker : g_showPing;
        if (drawMarker)
        {
            SIZE markerSize;
            ZeroMemory(&markerSize, sizeof(markerSize));
            GetTextExtentPoint32A(hdc, markerLine, lstrlenA(markerLine), &markerSize);
            int markerX = markerGutterX + max(0, (markerGutterWidth - markerSize.cx) / 2);
            DrawOverlayName(hdc, markerX, smallY, row, markerLine);
        }

        if (g_showName)
            DrawOverlayName(hdc, nameX, smallY, row, nameLine);

        y += lineHeight;
    }

    SelectObject(hdc, oldFont); // fonts are cached; do not delete

    if (hdc == backHdc)
    {
        BitBlt(paintHdc, 0, 0, paintWidth, paintHeight, backHdc, 0, 0, SRCCOPY);
        if (oldBitmap)
            SelectObject(backHdc, oldBitmap);
        DeleteObject(backBitmap);
        DeleteDC(backHdc);
    }
    else if (backHdc)
    {
        DeleteDC(backHdc);
    }

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        UpdateOverlayPosition();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT:
        PaintOverlay(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        DestroyWindow(hwnd);
        g_overlayWnd = NULL;
        return 0;

    case WM_DESTROY:
        g_overlayWnd = NULL;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}


static std::string JsonEscape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 8);

    for (size_t i = 0; i < input.size(); i++)
    {
        unsigned char c = (unsigned char)input[i];

        if (c == '\\')
            out += "\\\\";
        else if (c == '"')
            out += "\\\"";
        else if (c == '\b')
            out += "\\b";
        else if (c == '\f')
            out += "\\f";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20)
        {
            char tmp[8];
            wsprintfA(tmp, "\\u%04X", (unsigned int)c);
            out += tmp;
        }
        else
            out += (char)c;
    }

    return out;
}

static const char* RoleTextForRow(const OverlayRow& row)
{
    if (row.isLocal)
        return "Local";

    int liveKind = LiveChrTeamStyleKindForRow(row);
    if (liveKind == STICKY_ROUTE_HOST_LIVE_TYPE)
        return "Host";
    if (liveKind == STICKY_ROUTE_SUNLIGHT)
        return "SunlightSummon";
    if (liveKind == STICKY_ROUTE_WHITE_SUMMON)
        return "WhiteSummon";
    if (liveKind == STICKY_ROUTE_DRAGON)
        return "DragonSummon";
    if (liveKind == STICKY_ROUTE_FOREST)
        return "ForestHunter";
    if (liveKind == STICKY_ROUTE_GRAVELORD)
        return "GravelordServant";
    if (liveKind == STICKY_ROUTE_DARKMOON)
        return "DarkmoonInvader";
    if (liveKind == STICKY_ROUTE_BLUE_GENERIC)
        return "BlueRoute";
    if (liveKind == STICKY_ROUTE_RED_SUMMON)
        return "RedSummon";
    if (liveKind == STICKY_ROUTE_RED_INVADER)
        return "RedInvader";

    if (row.isPhantom == 0)
        return "Host";

    if (IsSunlightSummonWorldRow(row))
        return "SunlightSummon";
    if (IsDragonSummonWorldRow(row))
        return "DragonSummon";
    if (IsForestHunterWorldRow(row))
        return "ForestHunter";
    if (IsGravelordBlueWorldRow(row))
        return "GravelordServant";
    if (IsDarkmoonBlueWorldRow(row))
        return "DarkmoonInvader";
    if (IsGenericBlueAppearanceWorldRow(row))
        return "BlueRoute";
    if (IsRedSummonWorldRow(row))
        return "RedSummon";
    if (IsInvaderWorldRow(row))
        return "RedInvader";
    if (IsWhiteSummonWorldRow(row))
        return "WhiteSummon";

    if (PhantomTypeBase(row.phantomType) == 1)
        return "WhiteSummon";
    if (PhantomTypeBase(row.phantomType) == 2)
        return "BlueRoute";
    if (PhantomTypeBase(row.phantomType) == 3)
        return "Red";

    return "Phantom";
}

static std::string BuildOverlayJsonSnapshot()
{
    std::vector<OverlayRow> rows;

    if (g_rowsLockReady)
    {
        EnterCriticalSection(&g_rowsLock);
        rows = g_rows;
        LeaveCriticalSection(&g_rowsLock);
    }

    ULONGLONG now = GetTickCount64();
    LONG seq = InterlockedIncrement(&g_webSocketSendSeq);

    std::string json;
    char header[256];
    wsprintfA(
        header,
        "{\"type\":\"mctde_hp\",\"version\":1,\"seq\":%ld,\"timeMs\":%lu,\"players\":[",
        seq,
        (DWORD)now
    );
    json += header;

    for (size_t i = 0; i < rows.size(); i++)
    {
        const OverlayRow& r = rows[i];

        if (i > 0)
            json += ",";

        int displayHp = r.hp;
        if (r.hpOneUntilTick > now && r.hasHp)
            displayHp = 1;

        char fixedPart[512];
        wsprintfA(
            fixedPart,
            "{\"slotIndex\":%d,\"playerNo\":%d,\"isLocal\":%s,\"role\":\"%s\",\"invadeType\":%d,\"vowType\":%d,\"hasHp\":%s,\"hp\":%d,\"maxHp\":%d,\"hasPing\":%s,\"ping\":%d,\"hasTruePing\":%s,\"truePing\":%d,\"trueRtt\":%d,\"status\":%d,\"steamId\":\"%I64u\",",
            r.slotIndex,
            r.playerNo,
            r.isLocal ? "true" : "false",
            RoleTextForRow(r),
            r.invadeType,
            r.vowType,
            r.hasHp ? "true" : "false",
            displayHp,
            r.hpMax,
            r.hasPing ? "true" : "false",
            r.ping,
            r.hasTruePing ? "true" : "false",
            r.hasTruePing ? r.truePing : -1,
            r.hasTruePing ? r.trueRtt : -1,
            r.status,
            r.steamId
        );

        json += fixedPart;
        json += "\"slot\":\"";
        json += JsonEscape(r.slotName ? r.slotName : "");
        json += "\",\"name\":\"";
        json += JsonEscape(r.name);
        json += "\",\"worldName\":\"";
        json += JsonEscape(r.worldName);
        json += "\",\"pingSource\":\"";
        json += r.hasTruePing ? "true" : (r.hasPing ? "cached" : "none");
        json += "\"}";
    }

    json += "]}";
    return json;
}

static const char* GetEmbeddedOverlayHtml()
{
    return
        "<!doctype html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset='utf-8'>\n"
        "<title>MCTDE HP Overlay</title>\n"
        "<style>\n"
        "html,body{margin:0;padding:0;background:transparent;overflow:hidden;font-family:Tahoma,Arial,sans-serif;color:#fff;}\n"
        "#wrap{padding:18px;width:520px;}\n"
        ".row{margin:0 0 10px 0;text-shadow:2px 2px 2px #000;}\n"
        ".top{display:flex;justify-content:space-between;align-items:flex-end;font-size:22px;font-weight:700;}\n"
        ".role{font-size:14px;opacity:.8;margin-left:8px;}\n"
        ".hp{font-size:28px;font-weight:900;color:#70ff80;}\n"
        ".bar{height:14px;background:rgba(0,0,0,.65);border:1px solid rgba(255,255,255,.45);box-shadow:2px 2px 2px #000;}\n"
        ".fill{height:100%;background:#38d95a;width:0%;transition:width 80ms linear;}\n"
        ".dead .fill{background:#ff4040;}\n"
        ".dead .hp{color:#ff6060;}\n"
        "#status{font-size:13px;opacity:.7;text-shadow:1px 1px 1px #000;}\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<div id='wrap'><div id='status'>Connecting to MCTDE...</div><div id='players'></div></div>\n"
        "<script>\n"
        "const statusEl=document.getElementById('status');\n"
        "const playersEl=document.getElementById('players');\n"
        "let ws=null;\n"
        "function esc(s){return String(s||'').replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\\\"':'&quot;'}[c]));}\n"
        "function draw(msg){\n"
        "  const rows=msg.players||[];\n"
        "  statusEl.textContent=rows.length?'':'No live HP rows';\n"
        "  playersEl.innerHTML=rows.map(p=>{\n"
        "    const has=p.hasHp&&p.maxHp>0;\n"
        "    const hp=has?Math.max(0,p.hp):0;\n"
        "    const max=has?p.maxHp:0;\n"
        "    const pct=has?Math.max(0,Math.min(100,hp*100/max)):0;\n"
        "    const dead=has&&hp<=1?' dead':'';\n"
        "    const label=esc(p.name||p.worldName||('PlayerNo '+p.playerNo));\n"
        "    const role=esc(p.role||'');\n"
        "    const hpText=has?(hp+' / '+max):'--- / ---';\n"
        "    return `<div class='row${dead}'><div class='top'><div>${label}<span class='role'>${role}</span></div><div class='hp'>${hpText}</div></div><div class='bar'><div class='fill' style='width:${pct}%'></div></div></div>`;\n"
        "  }).join('');\n"
        "}\n"
        "function connect(){\n"
        "  const url='ws://'+location.host+'/ws';\n"
        "  statusEl.textContent='Connecting to '+url;\n"
        "  ws=new WebSocket(url);\n"
        "  ws.onopen=()=>{statusEl.textContent='Connected';};\n"
        "  ws.onmessage=e=>{try{draw(JSON.parse(e.data));}catch(err){statusEl.textContent='Bad JSON';}};\n"
        "  ws.onclose=()=>{statusEl.textContent='Disconnected; retrying...';setTimeout(connect,1000);};\n"
        "  ws.onerror=()=>{try{ws.close();}catch(e){}};\n"
        "}\n"
        "connect();\n"
        "</script>\n"
        "</body>\n"
        "</html>\n";
}

static std::string Base64Encode(const BYTE* data, size_t len)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3)
    {
        DWORD value = ((DWORD)data[i]) << 16;
        if (i + 1 < len)
            value |= ((DWORD)data[i + 1]) << 8;
        if (i + 2 < len)
            value |= ((DWORD)data[i + 2]);

        out += table[(value >> 18) & 0x3F];
        out += table[(value >> 12) & 0x3F];
        out += (i + 1 < len) ? table[(value >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? table[value & 0x3F] : '=';
    }

    return out;
}

struct Sha1Context
{
    DWORD state[5];
    DWORD count[2];
    BYTE buffer[64];
};

static DWORD Rol32(DWORD value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void Sha1Transform(DWORD state[5], const BYTE buffer[64])
{
    DWORD block[80];
    for (int i = 0; i < 16; i++)
    {
        block[i] = ((DWORD)buffer[i * 4 + 0] << 24) |
            ((DWORD)buffer[i * 4 + 1] << 16) |
            ((DWORD)buffer[i * 4 + 2] << 8) |
            ((DWORD)buffer[i * 4 + 3]);
    }

    for (int i = 16; i < 80; i++)
        block[i] = Rol32(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);

    DWORD a = state[0];
    DWORD b = state[1];
    DWORD c = state[2];
    DWORD d = state[3];
    DWORD e = state[4];

    for (int i = 0; i < 80; i++)
    {
        DWORD f;
        DWORD k;

        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        DWORD temp = Rol32(a, 5) + f + e + k + block[i];
        e = d;
        d = c;
        c = Rol32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void Sha1Init(Sha1Context* ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    ZeroMemory(ctx->buffer, sizeof(ctx->buffer));
}

static void Sha1Update(Sha1Context* ctx, const BYTE* data, size_t len)
{
    DWORD j = (ctx->count[0] >> 3) & 63;

    if ((ctx->count[0] += ((DWORD)len << 3)) < ((DWORD)len << 3))
        ctx->count[1]++;
    ctx->count[1] += ((DWORD)len >> 29);

    DWORD i = 0;
    if ((j + len) > 63)
    {
        DWORD partLen = 64 - j;
        CopyMemory(&ctx->buffer[j], data, partLen);
        Sha1Transform(ctx->state, ctx->buffer);

        for (i = partLen; i + 63 < len; i += 64)
            Sha1Transform(ctx->state, &data[i]);

        j = 0;
    }

    if (i < len)
        CopyMemory(&ctx->buffer[j], &data[i], len - i);
}

static void Sha1Final(BYTE digest[20], Sha1Context* ctx)
{
    BYTE finalCount[8];
    for (int i = 0; i < 8; i++)
        finalCount[i] = (BYTE)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);

    BYTE c = 0x80;
    Sha1Update(ctx, &c, 1);
    while (((ctx->count[0] >> 3) & 63) != 56)
    {
        c = 0x00;
        Sha1Update(ctx, &c, 1);
    }

    Sha1Update(ctx, finalCount, 8);

    for (int i = 0; i < 20; i++)
        digest[i] = (BYTE)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);

    ZeroMemory(ctx, sizeof(*ctx));
}

static std::string Sha1Base64(const std::string& text)
{
    Sha1Context ctx;
    BYTE digest[20];
    Sha1Init(&ctx);
    Sha1Update(&ctx, (const BYTE*)text.c_str(), text.size());
    Sha1Final(digest, &ctx);
    return Base64Encode(digest, sizeof(digest));
}

static bool SendAll(SOCKET s, const char* data, int len)
{
    int sentTotal = 0;
    while (sentTotal < len)
    {
        int sent = send(s, data + sentTotal, len - sentTotal, 0);
        if (sent <= 0)
            return false;
        sentTotal += sent;
    }
    return true;
}

static bool SendHttpText(SOCKET s, const char* contentType, const char* body)
{
    char header[512];
    wsprintfA(
        header,
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
        contentType,
        lstrlenA(body)
    );

    return SendAll(s, header, lstrlenA(header)) && SendAll(s, body, lstrlenA(body));
}

static std::string FindHttpHeaderValue(const std::string& request, const char* headerName)
{
    std::string needle = headerName;
    needle += ":";

    size_t pos = request.find(needle);
    if (pos == std::string::npos)
    {
        std::string lowerReq = request;
        std::string lowerNeedle = needle;
        for (size_t i = 0; i < lowerReq.size(); i++)
            lowerReq[i] = (char)tolower((unsigned char)lowerReq[i]);
        for (size_t i = 0; i < lowerNeedle.size(); i++)
            lowerNeedle[i] = (char)tolower((unsigned char)lowerNeedle[i]);
        pos = lowerReq.find(lowerNeedle);
    }

    if (pos == std::string::npos)
        return std::string();

    pos += needle.size();
    while (pos < request.size() && (request[pos] == ' ' || request[pos] == '\t'))
        pos++;

    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos)
        end = request.find('\n', pos);
    if (end == std::string::npos)
        end = request.size();

    std::string value = request.substr(pos, end - pos);
    while (!value.empty() && (value[value.size() - 1] == '\r' || value[value.size() - 1] == '\n' || value[value.size() - 1] == ' ' || value[value.size() - 1] == '\t'))
        value.erase(value.size() - 1);

    return value;
}

static bool SendWebSocketTextFrame(SOCKET s, const std::string& payload)
{
    BYTE header[10];
    int headerLen = 0;
    size_t len = payload.size();

    header[0] = 0x81;

    if (len <= 125)
    {
        header[1] = (BYTE)len;
        headerLen = 2;
    }
    else if (len <= 65535)
    {
        header[1] = 126;
        header[2] = (BYTE)((len >> 8) & 0xFF);
        header[3] = (BYTE)(len & 0xFF);
        headerLen = 4;
    }
    else
    {
        header[1] = 127;
        unsigned __int64 bigLen = (unsigned __int64)len;
        for (int i = 0; i < 8; i++)
            header[2 + i] = (BYTE)((bigLen >> (56 - i * 8)) & 0xFF);
        headerLen = 10;
    }

    if (!SendAll(s, (const char*)header, headerLen))
        return false;

    if (!payload.empty() && !SendAll(s, payload.c_str(), (int)payload.size()))
        return false;

    return true;
}

static void AddWebSocketClient(SOCKET client)
{
    if (!g_webSocketClientsLockReady)
    {
        closesocket(client);
        return;
    }

    DWORD timeout = 100;
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    EnterCriticalSection(&g_webSocketClientsLock);
    g_webSocketClients.push_back(client);
    InterlockedExchange(&g_webSocketClientCount, (LONG)g_webSocketClients.size());
    LeaveCriticalSection(&g_webSocketClientsLock);
}

static DWORD WINAPI WebSocketClientThread(LPVOID param)
{
    SOCKET client = (SOCKET)(uintptr_t)param;

    char buffer[8192];
    ZeroMemory(buffer, sizeof(buffer));
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0)
    {
        closesocket(client);
        return 0;
    }

    std::string request(buffer, received);

    if (request.find("GET /data.json") == 0)
    {
        std::string json = BuildOverlayJsonSnapshot();
        SendHttpText(client, "application/json; charset=utf-8", json.c_str());
        closesocket(client);
        return 0;
    }

    bool wantsWebSocket = (request.find("GET /ws") == 0) && (request.find("Upgrade: websocket") != std::string::npos || request.find("upgrade: websocket") != std::string::npos);

    if (wantsWebSocket)
    {
        std::string key = FindHttpHeaderValue(request, "Sec-WebSocket-Key");
        if (key.empty())
        {
            closesocket(client);
            return 0;
        }

        std::string accept = Sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

        if (!SendAll(client, response.c_str(), (int)response.size()))
        {
            closesocket(client);
            return 0;
        }

        AddWebSocketClient(client);
        return 0;
    }

    SendHttpText(client, "text/html; charset=utf-8", GetEmbeddedOverlayHtml());
    closesocket(client);
    return 0;
}

static DWORD WINAPI WebSocketAcceptThread(LPVOID)
{
    WriteLogLine("WebSocket accept thread started.");

    while (g_running && g_webSocketEnabled && g_webSocketListenSocket != INVALID_SOCKET)
    {
        SOCKET client = accept(g_webSocketListenSocket, NULL, NULL);
        if (client == INVALID_SOCKET)
        {
            Sleep(50);
            continue;
        }

        HANDLE thread = CreateThread(NULL, 0, WebSocketClientThread, (LPVOID)(uintptr_t)client, 0, NULL);
        if (thread)
            CloseHandle(thread);
        else
            closesocket(client);
    }

    WriteLogLine("WebSocket accept thread stopped.");
    return 0;
}

static DWORD WINAPI WebSocketSendThread(LPVOID)
{
    WriteLogLine("WebSocket send thread started.");

    while (g_running && g_webSocketEnabled)
    {
        std::string json = BuildOverlayJsonSnapshot();

        if (g_webSocketClientsLockReady)
        {
            EnterCriticalSection(&g_webSocketClientsLock);

            for (std::vector<SOCKET>::iterator it = g_webSocketClients.begin(); it != g_webSocketClients.end(); )
            {
                if (!SendWebSocketTextFrame(*it, json))
                {
                    closesocket(*it);
                    it = g_webSocketClients.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            InterlockedExchange(&g_webSocketClientCount, (LONG)g_webSocketClients.size());
            LeaveCriticalSection(&g_webSocketClientsLock);
        }

        Sleep(g_webSocketSendMs);
    }

    WriteLogLine("WebSocket send thread stopped.");
    return 0;
}

static void StartWebSocketServer()
{
    if (!g_webSocketEnabled)
    {
        WriteLogLine("WebSocket server disabled by INI.");
        return;
    }

    if (!g_webSocketClientsLockReady)
    {
        InitializeCriticalSection(&g_webSocketClientsLock);
        g_webSocketClientsLockReady = true;
    }

    WSADATA wsa;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsaResult != 0)
    {
        WriteLogf("WebSocket WSAStartup failed: %d", wsaResult);
        return;
    }

    g_webSocketListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_webSocketListenSocket == INVALID_SOCKET)
    {
        WriteLogf("WebSocket socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(g_webSocketListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)g_webSocketPort);

    if (bind(g_webSocketListenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        WriteLogf("WebSocket bind 127.0.0.1:%d failed: %d", g_webSocketPort, WSAGetLastError());
        closesocket(g_webSocketListenSocket);
        g_webSocketListenSocket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    if (listen(g_webSocketListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        WriteLogf("WebSocket listen failed: %d", WSAGetLastError());
        closesocket(g_webSocketListenSocket);
        g_webSocketListenSocket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    WriteLogf("WebSocket server listening: http://127.0.0.1:%d/overlay.html", g_webSocketPort);

    HANDLE acceptThread = CreateThread(NULL, 0, WebSocketAcceptThread, NULL, 0, NULL);
    if (acceptThread)
        CloseHandle(acceptThread);

    HANDLE sendThread = CreateThread(NULL, 0, WebSocketSendThread, NULL, 0, NULL);
    if (sendThread)
        CloseHandle(sendThread);
}

static void StopWebSocketServer()
{
    if (g_webSocketListenSocket != INVALID_SOCKET)
    {
        closesocket(g_webSocketListenSocket);
        g_webSocketListenSocket = INVALID_SOCKET;
    }

    if (g_webSocketClientsLockReady)
    {
        EnterCriticalSection(&g_webSocketClientsLock);
        for (size_t i = 0; i < g_webSocketClients.size(); i++)
            closesocket(g_webSocketClients[i]);
        g_webSocketClients.clear();
        InterlockedExchange(&g_webSocketClientCount, 0);
        LeaveCriticalSection(&g_webSocketClientsLock);
    }

    WSACleanup();
}


static DWORD WINAPI TruePingThread(LPVOID)
{
    WriteLogLine("TRUEPING thread started.");
    QueryPerformanceFrequency(&g_qpcFrequency);
    EnableTruePingTimerPeriod();
    ULONGLONG lastResolveLog = 0;
    ULONGLONG lastSendTick = 0;
    while (g_running)
    {
        if (!g_truePingEnabled)
        {
            Sleep(250);
            continue;
        }
        static bool loggedMode = false;
        if (!loggedMode)
        {
            WriteLogf("TRUEPING staged mode: sendEnabled=%d recvEnabled=%d ch=%d sendType=%d. When both are 0, v80 does not resolve/call SteamNetworking at all. Channel <16 is guarded by default; unmodded peers keep cached/session-table ping fallback; true ping uses low-latency polling and floor display by default.", g_truePingSendEnabled ? 1 : 0, g_truePingReceiveEnabled ? 1 : 0, g_truePingChannel, g_truePingSendType);
            loggedMode = true;
        }

        // Crashguard: if both sides are disabled, this thread should be inert.
        // v56 still resolved SteamNetworking in this mode, which made crash attribution noisy.
        if (!g_truePingSendEnabled && !g_truePingReceiveEnabled)
        {
            Sleep(250);
            continue;
        }

        if (g_truePingChannel < 16 && !g_truePingAllowGameChannel)
        {
            WriteLogf("TRUEPING runtime guard: channel %d is reserved/game-facing; disabling true-ping send/recv for this run.", g_truePingChannel);
            g_truePingSendEnabled = false;
            g_truePingReceiveEnabled = false;
            Sleep(1000);
            continue;
        }

        ULONGLONG nowResolve = GetTickCount64();
        bool logResolve = (lastResolveLog == 0 || nowResolve - lastResolveLog > 5000);
        if (!ResolveSteamNetworking(logResolve))
        {
            if (logResolve)
                lastResolveLog = nowResolve;
            Sleep(1000);
            continue;
        }

        int packets = 0;
        if (g_truePingReceiveEnabled)
        {
            while (packets < 64 && ReadOneMctpPacket())
                packets++;
        }
        ULONGLONG now = GetTickCount64();
        if (g_truePingSendEnabled && (lastSendTick == 0 || now - lastSendTick >= 100))
        {
            TickTruePingSends();
            lastSendTick = now;
        }
        if (g_truePingPollSleepMs > 0)
            Sleep((DWORD)g_truePingPollSleepMs);
        else
            SwitchToThread();
    }
    DisableTruePingTimerPeriod();
    WriteLogLine("TRUEPING thread stopped.");
    return 0;
}

static DWORD WINAPI PollThread(LPVOID)
{
    WriteLogLine("Poll thread started.");

    while (g_running)
    {
        std::vector<WorldActorRow> worldRows = ReadWorldActors();
        std::unordered_map<uint64_t, SteamNodeInfo> nodesById = ReadSteamNodesById();
        UpdateTruePingKnownRemoteSteamNodes(nodesById);

        // Every client can at least bind its own visible WORLD playerNo to its local Steam node.
        // This does not invent remote names; it just prevents the local row from staying Unknown.
        LearnLocalSelfIdentity(worldRows, nodesById);
        PruneIdentitiesForWorldRows(worldRows);

        std::vector<OverlayRow> rows = BuildOverlayRows(worldRows, nodesById);
        std::vector<OverlayRow> previousRows;

        if (g_rowsLockReady)
        {
            EnterCriticalSection(&g_rowsLock);
            previousRows = g_rows;
            LeaveCriticalSection(&g_rowsLock);
        }

        // Stabilization/roster-change compares against real (live) rows only, so the synthetic
        // "Disconnected" placeholders carried in g_rows don't churn the comparison every refresh.
        std::vector<OverlayRow> previousReal;
        previousReal.reserve(previousRows.size());
        for (size_t i = 0; i < previousRows.size(); i++)
            if (!previousRows[i].disconnected)
                previousReal.push_back(previousRows[i]);

        bool rosterChanged = OverlayRosterChanged(rows, previousReal);

        StabilizeRowsWithPrevious(rows, previousReal, rosterChanged);
        ApplyTruePingToRows(rows);
        StabilizeRowsWithPrevious(rows, previousReal, rosterChanged);

        DumpDebugRows(rows, worldRows, nodesById);

        if (g_rowsLockReady)
        {
            EnterCriticalSection(&g_rowsLock);

            ULONGLONG now = GetTickCount64();
            for (size_t i = 0; i < rows.size(); i++)
            {
                OverlayRow& newRow = rows[i];

                for (size_t j = 0; j < g_rows.size(); j++)
                {
                    const OverlayRow& oldRow = g_rows[j];
                    if (oldRow.playerNo == newRow.playerNo && oldRow.hpOneUntilTick > newRow.hpOneUntilTick)
                    {
                        newRow.hpOneUntilTick = oldRow.hpOneUntilTick;
                        break;
                    }
                }

                std::unordered_map<uintptr_t, ULONGLONG>::iterator forcedOne = g_oneHpUntilByChr.find(newRow.chr);
                if (forcedOne != g_oneHpUntilByChr.end() && forcedOne->second > newRow.hpOneUntilTick)
                    newRow.hpOneUntilTick = forcedOne->second;

                if (newRow.hasHp && newRow.hp == 1)
                {
                    newRow.hpOneUntilTick = now + g_hpOneLingerMs;
                    g_oneHpUntilByChr[newRow.chr] = newRow.hpOneUntilTick;
                }
            }

            for (std::unordered_map<uintptr_t, ULONGLONG>::iterator it = g_oneHpUntilByChr.begin(); it != g_oneHpUntilByChr.end(); )
            {
                if (it->second + 5000 < now)
                    it = g_oneHpUntilByChr.erase(it);
                else
                    ++it;
            }

            // Carry/forward "Disconnected" placeholder rows (left-without-dying), append survivors.
            ApplyDisconnectedRows(rows, previousRows);

            g_rows = rows;
            g_worldRows = worldRows;
            LeaveCriticalSection(&g_rowsLock);
        }

        Sleep(g_refreshMs);
    }

    WriteLogLine("Poll thread stopped.");
    return 0;
}

static DWORD WINAPI HpPollThread(LPVOID)
{
    WriteLogLine("HP poll thread started.");

    // Give the HP watcher the best chance of catching a one-frame 1 HP value.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (g_running)
    {
        if (g_showHp && g_rowsLockReady)
        {
            std::vector<WorldActorRow> worldRows = ReadWorldActors();

            EnterCriticalSection(&g_rowsLock);

            ULONGLONG now = GetTickCount64();

            for (size_t i = 0; i < g_rows.size(); i++)
            {
                OverlayRow& row = g_rows[i];

                if (row.disconnected)
                    continue; // placeholder row: no live HP to poll

                int oldHp = row.hp;
                int oldHpMax = row.hpMax;
                bool oldHadHp = row.hasHp;

                for (size_t j = 0; j < worldRows.size(); j++)
                {
                    const WorldActorRow& w = worldRows[j];
                    if (!w.valid || w.playerNo != row.playerNo)
                        continue;

                    if (!w.hasHp)
                        break;

                    row.hasHp = w.hasHp;
                    row.hp = w.hp;
                    row.hpMax = w.hpMax;
                    row.hpCurrentOffset = w.hpCurrentOffset;
                    row.hpMaxOffset = w.hpMaxOffset;

                    if (row.hasHp)
                    {
                        bool literalOneHp = (row.hp == 1);

                        bool instantRefillHit = false;
                        if (g_hpDetectInstantRefill && oldHadHp && oldHp > 1 && row.hp > oldHp)
                        {
                            int gain = row.hp - oldHp;
                            int maxHp = row.hpMax > 0 ? row.hpMax : oldHpMax;
                            bool nearFull = (maxHp > 0 && row.hp >= maxHp - 2);

                            if (gain >= g_hpInstantRefillMinGain && nearFull)
                                instantRefillHit = true;
                        }

                        if (literalOneHp || instantRefillHit)
                        {
                            row.hpOneUntilTick = now + g_hpOneLingerMs;
                            g_oneHpUntilByChr[row.chr] = row.hpOneUntilTick;
                        }
                    }

                    break;
                }
            }

            LeaveCriticalSection(&g_rowsLock);
        }

        Sleep(g_hpPollMs);
    }

    WriteLogLine("HP poll thread stopped.");
    return 0;
}

// ------------------------------------------------------------
// Direct3D backend: rasterize the same rows into a tight 32-bit BGRA bitmap
// (reusing the exact GDI text/outline drawing) and hand it to D3DOverlay, which
// draws it as a single in-frame quad. No window, no DWM composition.
// ------------------------------------------------------------
static void RenderOverlayToDIBAndSubmit()
{
    // Master hotkey toggle: submit an empty bitmap so the in-frame overlay clears.
    if (!g_overlayVisible)
    {
        D3DOverlay_Submit(NULL, 0, 0, (int)g_corner, g_paddingX, g_paddingY);
        return;
    }

    std::vector<OverlayRow> rows;
    if (g_rowsLockReady)
    {
        EnterCriticalSection(&g_rowsLock);
        rows = g_rows;
        LeaveCriticalSection(&g_rowsLock);
    }

    {
        static int s_lastRowCount = -1;
        if ((int)rows.size() != s_lastRowCount)
        {
            WriteLogf("[mctde-d3d] submit rows %d -> %d", s_lastRowCount, (int)rows.size());
            s_lastRowCount = (int)rows.size();
        }
    }

    if (rows.empty())
    {
        D3DOverlay_Submit(NULL, 0, 0, (int)g_corner, g_paddingX, g_paddingY);
        return;
    }

    HFONT smallFont = CreateFontA(
        g_fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_fontFace);
    if (smallFont == NULL)
        smallFont = CreateFontA(g_fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");

    HFONT hpFont = CreateFontA(
        g_hpFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_fontFace);
    if (hpFont == NULL)
        hpFont = CreateFontA(g_hpFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");

    HDC screenDC = GetDC(NULL);
    HDC hdc = CreateCompatibleDC(screenDC);

    HFONT oldFont = (HFONT)SelectObject(hdc, hpFont);
    TEXTMETRICA hpTm; ZeroMemory(&hpTm, sizeof(hpTm));
    GetTextMetricsA(hdc, &hpTm);
    SIZE hpMaxSize; ZeroMemory(&hpMaxSize, sizeof(hpMaxSize));
    GetTextExtentPoint32A(hdc, "9999HP", 6, &hpMaxSize);
    // Disconnected placeholder rows render "Disconnected" in the HP column; widen it to fit.
    for (size_t di = 0; di < rows.size(); di++)
    {
        if (rows[di].disconnected)
        {
            SIZE dcSize; ZeroMemory(&dcSize, sizeof(dcSize));
            GetTextExtentPoint32A(hdc, "Disconnected", 12, &dcSize);
            if (dcSize.cx > hpMaxSize.cx) hpMaxSize.cx = dcSize.cx;
            break;
        }
    }

    SelectObject(hdc, smallFont);
    TEXTMETRICA smallTm; ZeroMemory(&smallTm, sizeof(smallTm));
    GetTextMetricsA(hdc, &smallTm);
    SIZE markerMaxSize; ZeroMemory(&markerMaxSize, sizeof(markerMaxSize));
    if (g_showPing)
        GetTextExtentPoint32A(hdc, "999MS(C)", 8, &markerMaxSize);
    if (g_showLocalMarker)
    {
        SIZE localMarkerMaxSize; ZeroMemory(&localMarkerMaxSize, sizeof(localMarkerMaxSize));
        GetTextExtentPoint32A(hdc, "\\[T]/", 5, &localMarkerMaxSize);
        if (localMarkerMaxSize.cx > markerMaxSize.cx)
            markerMaxSize.cx = localMarkerMaxSize.cx;
    }

    int lineHeight = g_lineHeight;
    if (lineHeight < hpTm.tmHeight + 2)
        lineHeight = hpTm.tmHeight + 2;

    // HP column is gated by [HP] Enabled (subsystem) AND [Overlay] ShowHp (display).
    bool drawHpField = g_showHp && g_showHpField;
    // The marker gutter collapses to zero when neither marker can ever appear.
    bool anyMarker = g_showPing || g_showLocalMarker;
    int hpFieldWidth = drawHpField ? (hpMaxSize.cx + 6) : 0;
    int markerGutterWidth = anyMarker ? (markerMaxSize.cx + g_markerGutterExtra) : 0;
    int nameX = hpFieldWidth + markerGutterWidth;
    int totalLines = (int)rows.size();
    int blockHeight = totalLines * lineHeight;

    // Measure the widest name so the bitmap is sized tightly (no clipping).
    int maxNameWidth = 0;
    if (g_showName)
    {
        for (size_t i = 0; i < rows.size(); i++)
        {
            const char* nm = rows[i].name.c_str();
            int len = lstrlenA(nm);
            if (len <= 0)
                continue;
            SIZE ns; ZeroMemory(&ns, sizeof(ns));
            GetTextExtentPoint32A(hdc, nm, len, &ns);
            if (ns.cx > maxNameWidth)
                maxNameWidth = ns.cx;
        }
    }

    int contentW = nameX + maxNameWidth + 8;
    int contentH = blockHeight + 2;
    if (contentW < 1) contentW = 1;
    if (contentH < 1) contentH = 1;

    // Top-down 32bpp DIB so row 0 is the top scanline.
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = contentW;
    bmi.bmiHeader.biHeight = -contentH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

    if (dib != NULL && bits != NULL)
    {
        HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, dib);

        RECT rcAll = { 0, 0, contentW, contentH };
        HBRUSH keyBrush = CreateSolidBrush(OVERLAY_TRANSPARENT_KEY);
        FillRect(hdc, &rcAll, keyBrush);
        DeleteObject(keyBrush);

        SetBkMode(hdc, TRANSPARENT);

        int x = 0;
        int y = 0;
        for (size_t i = 0; i < rows.size(); i++)
        {
            const OverlayRow& row = rows[i];

            char pingText[32];
            char pingSourceText[8];
            char hpText[32];
            char hpLine[64];
            char markerLine[64];
            const char* nameLine = row.name.c_str();

            if (g_showPing && row.hasPing && row.ping >= 0)
                FormatFixedField(pingText, sizeof(pingText), row.ping, 3, "---");
            else
                lstrcpyA(pingText, "---");

            pingSourceText[0] = 0;
            if (g_truePingShowSourceMarker && g_showPing && row.hasPing && row.ping >= 0 && !row.hasTruePing && !row.isLocal)
                lstrcpyA(pingSourceText, "(C)");

            if (g_showHp && row.hasHp && row.hp >= 0)
                FormatFixedField(hpText, sizeof(hpText), row.hp, 4, "----");
            else
                lstrcpyA(hpText, "----");

            wsprintfA(hpLine, "%sHP", hpText);
            if (row.isLocal)
                lstrcpyA(markerLine, "\\[T]/");
            else
                wsprintfA(markerLine, "%sMS%s", pingText, pingSourceText);

            if (row.disconnected)
            {
                // Placeholder for a player who left without dying: "Disconnected    ---MS    Name".
                lstrcpyA(hpLine, "Disconnected");
                lstrcpyA(markerLine, "---MS");
            }

            int hpY = y + ((lineHeight - hpTm.tmHeight) / 2);
            int smallY = y + ((lineHeight - smallTm.tmHeight) / 2);
            int markerGutterX = x + hpFieldWidth;

            if (drawHpField)
            {
                SelectObject(hdc, hpFont);
                DrawOverlayHp(hdc, x, hpY, row, hpLine);
            }

            SelectObject(hdc, smallFont);

            bool drawMarker = row.isLocal ? g_showLocalMarker : g_showPing;
            if (drawMarker)
            {
                SIZE markerSize; ZeroMemory(&markerSize, sizeof(markerSize));
                GetTextExtentPoint32A(hdc, markerLine, lstrlenA(markerLine), &markerSize);
                int markerX = markerGutterX + max(0, (markerGutterWidth - markerSize.cx) / 2);
                DrawOverlayName(hdc, markerX, smallY, row, markerLine);
            }

            if (g_showName)
                DrawOverlayName(hdc, nameX, smallY, row, nameLine);

            y += lineHeight;
        }

        GdiFlush();

        // GDI leaves the alpha byte at 0. Convert the color-key to transparent and
        // make every other pixel opaque, producing valid A8R8G8B8 (0xAARRGGBB).
        DWORD* px = (DWORD*)bits;
        int count = contentW * contentH;
        for (int i = 0; i < count; i++)
        {
            DWORD c = px[i];
            if ((c & 0x00FFFFFF) == 0x00FF00FF) // OVERLAY_TRANSPARENT_KEY = magenta
                px[i] = 0x00000000;
            else
                px[i] = c | 0xFF000000;
        }

        D3DOverlay_Submit(bits, contentW, contentH, (int)g_corner, g_paddingX, g_paddingY);

        SelectObject(hdc, oldBmp);
    }

    SelectObject(hdc, oldFont);
    if (dib) DeleteObject(dib);
    DeleteObject(smallFont);
    DeleteObject(hpFont);
    DeleteDC(hdc);
    ReleaseDC(NULL, screenDC);
}

static DWORD WINAPI D3DSubmitThread(LPVOID)
{
    WriteLogLine("D3D overlay submit thread started.");
    int tick = 0;
    while (g_running)
    {
        if ((tick++ % 16) == 0)
            LoadConfig(false); // refresh config ~1Hz instead of every submit
        RenderOverlayToDIBAndSubmit();
        Sleep(g_d3dSubmitMs);
    }
    WriteLogLine("D3D overlay submit thread stopped.");
    return 0;
}

static DWORD WINAPI OverlayThread(LPVOID)
{
    WriteLogLine("Overlay thread started.");

    if (g_renderBackend == 1)
    {
        WriteLogLine("Overlay backend = d3d (in-frame). Skipping layered window.");
        return D3DSubmitThread(NULL);
    }

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = "MCTDE_NetOverlay_Window_Class";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClassExA(&wc);

    g_gameWnd = FindGameWindow();
    if (g_gameWnd != NULL)
        WriteLogf("Initial game window: %08X", (DWORD)g_gameWnd);
    else
        WriteLogLine("Initial game window not found. Overlay will stay hidden until found.");

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_forceTopmost)
        exStyle |= WS_EX_TOPMOST;

    HWND owner = NULL;
    if (!g_forceTopmost && g_gameWnd != NULL && IsWindow(g_gameWnd))
        owner = g_gameWnd;

    g_overlayWnd = CreateWindowExA(
        exStyle,
        wc.lpszClassName,
        "MCTDE Net Overlay v40",
        WS_POPUP,
        0,
        0,
        100,
        100,
        owner,
        NULL,
        g_hInstance,
        NULL
    );

    if (g_overlayWnd == NULL)
    {
        WriteLogf("Failed to create overlay window. GetLastError=%lu", GetLastError());
        return 0;
    }

    g_overlayOwnerWnd = owner;
    WriteLogf("Created overlay window: %08X owner=%08X", (DWORD)g_overlayWnd, (DWORD)g_overlayOwnerWnd);

    SetLayeredWindowAttributes(g_overlayWnd, OVERLAY_TRANSPARENT_KEY, 255, LWA_COLORKEY);
    SetTimer(g_overlayWnd, 1, (UINT)(g_repaintMs > 0 ? g_repaintMs : 66), NULL);

    UpdateOverlayPosition();
    ShowWindow(g_overlayWnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (g_running && GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    WriteLogLine("Overlay thread stopped.");
    return 0;
}

// Polls the configured virtual-key and flips g_overlayVisible on a rising edge.
// Only fires while the game window is focused so the bind never triggers while the
// user is alt-tabbed and typing elsewhere.
static DWORD WINAPI HotkeyThread(LPVOID)
{
    WriteLogLine("Hotkey thread started.");
    bool prevDown = false;
    while (g_running)
    {
        int key = g_toggleKey;
        if (key > 0 && key <= 255)
        {
            bool down = (GetAsyncKeyState(key) & 0x8000) != 0;

            bool gameFocused = false;
            HWND fg = GetForegroundWindow();
            if (fg != NULL)
            {
                DWORD fgPid = 0;
                GetWindowThreadProcessId(fg, &fgPid);
                gameFocused = (fgPid == GetCurrentProcessId());
            }

            // Optional modifier must be held at the moment the key goes down.
            bool modOk = (g_toggleModifier <= 0) ||
                ((GetAsyncKeyState(g_toggleModifier) & 0x8000) != 0);

            if (down && !prevDown && gameFocused && modOk)
            {
                g_overlayVisible = !g_overlayVisible;
                WriteLogf("Overlay toggled %s via hotkey (vk=0x%02X mod=0x%02X).",
                    g_overlayVisible ? "ON" : "OFF", key, g_toggleModifier);
            }
            prevDown = down;
        }
        else
        {
            prevDown = false;
        }
        Sleep(30);
    }
    WriteLogLine("Hotkey thread stopped.");
    return 0;
}

// Anti-zombie watchdog. DS1 (with DSFix + this mod's background threads) can leave a lingering
// DARKSOULS.exe after the window closes if the game doesn't fully ExitProcess. Once we've seen
// the game window, we watch it; when it's destroyed AND no game window can be found for a few
// consecutive seconds (i.e. the game is really closing, not minimized or briefly recreated),
// we force a clean process exit. IsWindow() stays true while minimized, so this won't fire then.
static DWORD WINAPI WatchdogThread(LPVOID)
{
    WriteLogLine("Watchdog thread started.");

    HWND gw = NULL;
    while (g_running && gw == NULL)
    {
        gw = FindGameWindow();
        if (gw == NULL)
            Sleep(750);
    }
    if (!g_running)
        return 0;

    WriteLogf("Watchdog: tracking game window %08X", (DWORD)(uintptr_t)gw);

    int goneStreak = 0;
    while (g_running)
    {
        Sleep(1000);

        if (!g_exitWithGame)
        {
            goneStreak = 0;
            continue;
        }

        if (IsWindow(gw))           // true even while minimized; false only once destroyed
        {
            goneStreak = 0;
            continue;
        }

        HWND again = FindGameWindow();
        if (again != NULL)          // game recreated its window -> keep tracking the new one
        {
            gw = again;
            goneStreak = 0;
            continue;
        }

        if (++goneStreak >= 3)      // ~3s with no game window at all -> game is closing
        {
            WriteLogLine("Watchdog: game window gone; forcing process exit to avoid a zombie process.");
            g_running = false;
            ExitProcess(0);
        }
    }

    WriteLogLine("Watchdog thread stopped.");
    return 0;
}

static DWORD WINAPI DelayedStartThread(LPVOID)
{
    WriteLogLine("Delayed start thread started. Sleeping before overlay init.");
    Sleep(8000);

    WriteLogLine("Delayed start awake. Starting overlay systems.");
    LoadConfig(true);

    if (!g_rowsLockReady)
    {
        InitializeCriticalSection(&g_rowsLock);
        g_rowsLockReady = true;
    }

    if (!g_identityLockReady)
    {
        InitializeCriticalSection(&g_identityLock);
        ZeroMemory(g_identityByPlayerNo, sizeof(g_identityByPlayerNo));
        g_identityLockReady = true;
    }

    if (!g_truePingLockReady)
    {
        InitializeCriticalSection(&g_truePingLock);
        g_truePingLockReady = true;
    }

    InstallHooks();

    g_running = true;

    HANDLE poll = CreateThread(NULL, 0, PollThread, NULL, 0, NULL);
    if (poll)
        CloseHandle(poll);

    HANDLE hpPoll = CreateThread(NULL, 0, HpPollThread, NULL, 0, NULL);
    if (hpPoll)
        CloseHandle(hpPoll);

    HANDLE truePing = CreateThread(NULL, 0, TruePingThread, NULL, 0, NULL);
    if (truePing)
        CloseHandle(truePing);

    HANDLE hotkey = CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
    if (hotkey)
        CloseHandle(hotkey);

    HANDLE watchdog = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    if (watchdog)
        CloseHandle(watchdog);

    StartWebSocketServer();

    HANDLE overlay = CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
    if (overlay)
        CloseHandle(overlay);

    WriteLogLine("Delayed start finished.");
    return 0;
}

extern "C" __declspec(dllexport) void initialize_plugin()
{
    BuildPaths();
    LoadLoggingSetting();

    if (g_loggingEnabled)
        DeleteFileA(g_logPath);

    WriteLogLine("----------------------------------------");
    WriteLogLine("MCTDE NetOverlay v80 narrow centered gutter initialize_plugin called.");
    WriteLogf("EXE base=%08X", (DWORD)ExeBase());
    WriteLogf("INI path=%s", g_iniPath);

    HANDLE thread = CreateThread(NULL, 0, DelayedStartThread, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
}

#ifdef MCTDE_LINK_SINGLE_DLL
extern "C" void McTDE_NetOverlay_OnProcessAttach(HMODULE hModule)
{
    g_hInstance = (HINSTANCE)hModule;
}

extern "C" void McTDE_NetOverlay_OnProcessDetach()
{
    g_running = false;
    DisableTruePingTimerPeriod();
    StopWebSocketServer();
    D3DOverlay_Shutdown();
}

// Lets the D3D overlay module route diagnostics into the same log file
// (only writes when [Settings] EnableLogging=1).
extern "C" void McTDE_NetOverlay_Log(const char* text)
{
    if (text)
        WriteLogLine(text);
}
#else
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInstance = (HINSTANCE)hModule;
        DisableThreadLibraryCalls(hModule);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = false;
        DisableTruePingTimerPeriod();
        StopWebSocketServer();
    }

    return TRUE;
}
#endif
