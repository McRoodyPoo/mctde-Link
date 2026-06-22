/*
    MorePhantoms  -  mctde-Link (built into d3d9.dll)

    Host glue for the MorePhantoms patch modules: reads the [MorePhantoms] ini
    section, shows the launch-time Yes/No opt-in prompt, drives Prepare/Verify/
    Apply, routes diagnostics into mctde-Link's log, and reverts every patch on
    teardown.

    The phantom-cap offset facts live in MorePhantomsStage1.cpp / MorePhantomsStage2.cpp,
    derived from Metal-Crow's reverse engineering (MultiPhantom / Dark Souls Overhaul).
    This is a rewrite for mctde-Link's patch engine, with attribution; no source text is
    intentionally copied.

    SAFE DEFAULT: [MorePhantoms] VerifyOnly=1. Until that is flipped to 0, the engine
    only logs a VERIFY report and never writes -- so a wrong offset can never corrupt
    the running game. Keep it at 1 until the log shows 0 mismatches AND Stage 2
    (offset-shift trampolines) is in place; Stage 1 alone is not a stable >4 session.
*/
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "MorePhantoms.h"
#include "PatchEngine.h"
#include "MorePhantomsStage1.h"
#include "MorePhantomsStage2.h"

// mctde-Link's shared logger (writes to the overlay log when [Settings] EnableLogging=1).
extern "C" void McTDE_NetOverlay_Log(const char* text);

namespace {

bool g_promptResolved = false;   // Mode / prompt has been evaluated this session
bool g_userEnabled    = false;   // result: should MorePhantoms patch this run?
bool g_applied        = false;   // Stage 1 patches actually committed
bool g_startDone      = false;   // MorePhantoms_Start() has already run (idempotent)
int  g_activeCount    = 4;       // live session slot count: 4 (stock) until patches commit

// ---- paths -----------------------------------------------------------------
// Resolve the directory of THIS module (d3d9.dll) so the ini path matches the
// host's g_iniPath regardless of init order -- the prompt runs before the
// NetOverlay module builds its own paths.
std::string ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ModuleDir), &self);

    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    else lstrcpyA(path, ".\\");
    return std::string(path);
}

std::string IniPath() { return ModuleDir() + "mctde-link.ini"; }
std::string LogPath() { return ModuleDir() + "MorePhantoms.log"; }

// ---- log sink --------------------------------------------------------------
// We gate very early (at Direct3DCreate9, before the host's logging is configured),
// so MorePhantoms keeps its OWN log file -- the VERIFY report is captured regardless
// of host init order or [Settings] EnableLogging. We also forward to the host log
// (when enabled) and the debugger.
void LogSink(const std::string& msg) {
    const std::string line = "[MorePhantoms] " + msg;

    static const std::string path = LogPath();
    HANDLE f = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const std::string out = line + "\r\n";
        DWORD wrote = 0;
        WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr);
        CloseHandle(f);
    }

    McTDE_NetOverlay_Log(line.c_str());
    OutputDebugStringA((line + "\n").c_str());
}

void EnsureLogSink() {
    static bool once = false;
    if (!once) {
        DeleteFileA(LogPath().c_str()); // start each session with a fresh log
        mp::SetLogSink(&LogSink);
        once = true;
    }
}

enum class Mode { Ask, On, Off };

Mode ReadMode(const std::string& ini) {
    char buf[16] = { 0 };
    GetPrivateProfileStringA("MorePhantoms", "Mode", "Ask", buf, sizeof(buf), ini.c_str());
    if (_stricmp(buf, "On")  == 0) return Mode::On;
    if (_stricmp(buf, "Off") == 0) return Mode::Off;
    return Mode::Ask;
}

// True if a Phantom_Break-style cap mod is present (loaded module OR configured as a
// chainload DLL). Both mods patch the same phantom-cap offsets, so running them together
// double-patches and crashes -- we refuse to apply in that case.
bool PhantomBreakPresent(const std::string& ini) {
    if (GetModuleHandleA("Phantom_Break.dll") || GetModuleHandleA("phantom_break.dll"))
        return true;
    for (int i = 0; i < 10; ++i) {
        char key[16]; sprintf_s(key, sizeof(key), "GenericDLL%d", i);
        char val[MAX_PATH] = { 0 };
        GetPrivateProfileStringA("DLLs", key, "", val, sizeof(val), ini.c_str());
        std::string s = val;
        for (char& c : s) c = (char)tolower((unsigned char)c);
        if (s.find("phantom_break") != std::string::npos) return true;
    }
    return false;
}

// True when running under Wine/Proton (Linux). ntdll exports wine_get_version there.
bool RunningUnderWine() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

} // namespace

void MorePhantoms_Prompt() {
    if (g_promptResolved) return;
    EnsureLogSink();

    const std::string ini = IniPath();
    const Mode mode = ReadMode(ini);

    if (mode == Mode::Off) {
        g_userEnabled = false;
        g_promptResolved = true;
        mp::Log("Mode=Off; MorePhantoms disabled (vanilla matchmaking).");
        return;
    }
    if (mode == Mode::On) {
        g_userEnabled = true;
        g_promptResolved = true;
        mp::Log("Mode=On; MorePhantoms enabled without prompt.");
        return;
    }

    // Under Proton/Wine, a modal MessageBox here -- on the D3D-init thread, before any
    // window exists -- hangs or crashes startup, so we never show it. Treat Ask as disabled
    // and let Linux players opt in explicitly via Mode=On.
    if (RunningUnderWine()) {
        g_userEnabled = false;
        g_promptResolved = true;
        mp::Log("Proton/Wine detected: the Ask prompt is unsupported here, so MorePhantoms is "
                "OFF. Set [MorePhantoms] Mode=On (or Off) in mctde-link.ini to choose explicitly.");
        return;
    }

    // Mode == Ask: modal opt-in before the overlay/net modules initialize.
    // No is the DEFAULT button (MB_DEFBUTTON2): the normal way to play is with
    // MorePhantoms DISABLED -- enabling it is a deliberate, special-occasion choice that
    // segregates you into a separate (and, until Stage 2, less stable) pool.
    const int answer = MessageBoxA(
        NULL,
        "Enable MorePhantoms?\r\n\r\n"
        "This raises the co-op / invasion phantom cap above the stock 4.\r\n\r\n"
        "While enabled you will ONLY be able to connect with other players who also "
        "have MorePhantoms enabled (a separate matchmaking pool).\r\n\r\n"
        "Most of the time you should choose NO and play normally with everyone. Only "
        "choose Yes for a planned MorePhantoms session with others who also have it on.",
        "mctde-Link - MorePhantoms",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON2);

    g_userEnabled = (answer == IDYES);
    g_promptResolved = true;
    mp::Log(g_userEnabled ? "Prompt: player chose YES (MorePhantoms enabled)."
                          : "Prompt: player chose NO (vanilla matchmaking).");
}

void MorePhantoms_Start() {
    EnsureLogSink();
    if (g_startDone) return;                       // idempotent: gate runs exactly once
    g_startDone = true;
    if (!g_promptResolved) MorePhantoms_Prompt();  // safety: never start un-gated

    const std::string ini = IniPath();
    const int maxPhantoms = GetPrivateProfileIntA("MorePhantoms", "MaxPhantoms", 18, ini.c_str());
    const int verifyOnly  = GetPrivateProfileIntA("MorePhantoms", "VerifyOnly",  1, ini.c_str());

    // NetworkVersion (the matchmaking pool key) is read as a string so both decimal ("77")
    // and hex ("0x4D") are accepted. Default 0x4D. Vanilla retail is 0x2E.
    int netVer = 0x4D;
    {
        char raw[16] = { 0 };
        GetPrivateProfileStringA("MorePhantoms", "NetworkVersion", "0x4D", raw, sizeof(raw), ini.c_str());
        const long v = strtol(raw, nullptr, 0); // base 0 -> auto-detect 0x.. or decimal
        if (v >= 0 && v <= 0xFF) netVer = (int)v;
    }

    // Game internal memory pool, in MB. 0 leaves it stock (~10 MB). Clamp to 255 MB so the
    // byte value stays under the engine's safe max (0x0FFFFFFF). Stage 1 only patches it
    // when the requested size exceeds the stock pool.
    int poolMB = GetPrivateProfileIntA("MorePhantoms", "MemoryPoolMB", 192, ini.c_str());
    if (poolMB < 0)   poolMB = 0;
    if (poolMB > 255) poolMB = 255;
    const uint32_t poolBytes = (uint32_t)poolMB << 20;

    mp::Log("----------------------------------------");
    char verHex[8]; sprintf_s(verHex, sizeof(verHex), "0x%02X", netVer);
    mp::Log("MorePhantoms_Start: enabled=" + std::to_string(g_userEnabled ? 1 : 0)
            + " MaxPhantoms=" + std::to_string(maxPhantoms)
            + " NetworkVersion=" + verHex
            + " MemoryPoolMB=" + std::to_string(poolMB)
            + " VerifyOnly=" + std::to_string(verifyOnly));

    if (!g_userEnabled) {
        mp::Log("Not enabled this run; leaving the game untouched.");
        return;
    }

    mp::MorePhantomsConfig cfg;
    cfg.maxPhantoms     = (uint8_t)maxPhantoms;
    cfg.networkVersion  = (uint8_t)netVer;
    cfg.memoryPoolBytes = poolBytes;

    if (!mp::MorePhantomsStage1_Prepare(cfg)) {
        mp::Log("Prepare failed; aborting (no changes made).");
        return;
    }

    const int mismatches = mp::MorePhantomsStage1_Verify();

    if (verifyOnly) {
        mp::Log("VerifyOnly mode: leaving the game untouched. "
                "Set [MorePhantoms] VerifyOnly=0 once the report above shows 0 mismatches.");
        return;
    }

    // Refuse if Phantom_Break (or a similar cap mod) is active: both patch the same sites,
    // and applying on top of it double-patches the game -> crash.
    if (PhantomBreakPresent(ini)) {
        mp::Log("Phantom_Break detected (loaded module or [DLLs] chainload entry). Refusing "
                "to apply MorePhantoms -- running both double-patches the game and crashes. "
                "Disable Phantom_Break to use MorePhantoms.");
        return;
    }

    if (mismatches != 0) {
        mp::Log("Refusing to apply: " + std::to_string(mismatches)
                + " offset mismatch(es). Check your exe/version before forcing this.");
        return;
    }

    if (mp::MorePhantomsStage1_Apply()) {
        g_applied = true;
        g_activeCount = maxPhantoms;   // overlay now reads this many phantom slots
        mp::Log("Stage 1 static patches applied.");
        // Stage 2: offset-shift trampolines + caves + deferred AoB (makes >4 stable).
        // Calibrated for N=18; installs only at that count, else logs + skips.
        mp::MorePhantomsStage2_Install((uint8_t)maxPhantoms);
    } else {
        mp::Log("Apply failed; engine rolled back.");
    }
}

void MorePhantoms_Restore() {
    // Idempotent: safe to call from OnProcessDetach and the watchdog teardown path.
    // Unwind Stage 2 (installed last) before Stage 1.
    mp::MorePhantomsStage2_Restore();
    mp::MorePhantomsStage1_Restore();
    g_applied = false;
    g_activeCount = 4; // back to stock layout
}

int MorePhantoms_ActivePhantomCount() {
    return g_activeCount;
}
