/*
    HideSoulCounter  -  mctde-Link (built into d3d9.dll)

    Hides the bottom-right HUD soul counter, reversibly. Two modes via INI:
      - default:      hide just the NUMBER (+ the "+N" gain popup); the dark plate/box stays.
      - HideBox=1:    hide the ENTIRE bottom-right soul display (box + icon + number + popup),
                      leaving the corner empty. See "patch site 3 (HideBox)" below.

    ---- How it was found (Ghidra static RE of the PTDE DARKSOULS.exe) -----------------
    The bottom-right counter is the in-game F20 HUD's "soul_param" element; the digits live
    in a text element named "text_param_soul". The soul_param widget's per-frame update
    method (Ghidra VA 0x00FADA70) reads the real soul total ([[WorldBase]+8]+0x8C), runs the
    rolling-number animation, then -- ONLY when the displayed value changed -- formats it into
    the text element. The guard is:

        00FADD1E  39 BE 04 01 00 00   cmp  [esi+0x104], edi   ; displayed value changed?
        00FADD24  74 73               jz   0x00FADD99         ; if UNCHANGED, skip the draw
        00FADD26  8B 9E F4 00 00 00   mov  ebx, [esi+0xF4]    ; ebx = text_param_soul handle
        ...                                                   ; build digit string + apply

    Flipping the JZ (0x74) to an unconditional JMP (0xEB) makes the game ALWAYS take the
    "value unchanged" path -- which it already executes ~every frame your souls are static --
    so the digit text element is never populated. Safe by construction: we only force a code
    path the engine runs constantly. The soul icon (a separate element) is unaffected.

    ---- Why AoB, not the absolute RVA ------------------------------------------------
    The site is at RVA 0xBADD24 in the analyzed build, but per mctde-Link house rules we
    locate it by a signature that is unique in the image (verified: exactly one match) so the
    patch survives relocation and minor build drift. The expected RVA is logged as a sanity
    cross-check only.

    SAFE DEFAULT: [HideSoulCounter] Enabled=0, VerifyOnly=1. Nothing is written until BOTH
    Enabled=1 and VerifyOnly=0. Until then we only emit a VERIFY report to HideSoulCounter.log.
*/
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>

#include "HideSoulCounter.h"
#include "PatchEngine.h"

// mctde-Link's shared logger (writes to the overlay log when [Settings] EnableLogging=1).
extern "C" void McTDE_NetOverlay_Log(const char* text);

namespace {

// ---- patch site 1: the persistent soul-counter NUMBER ----------------------
//   cmp [esi+0x104],edi ; jz +0x73 ; mov ebx,[esi+0xF4]
// The JZ opcode is at index 6. We keep the trailing rel8 (0x73) as matched.
const uint8_t  SIG[]      = { 0x39,0xBE,0x04,0x01,0x00,0x00, 0x74,0x73, 0x8B,0x9E,0xF4,0x00,0x00,0x00 };
const size_t   SIG_LEN    = sizeof(SIG);
const size_t   JZ_INDEX   = 6;          // offset of the 0x74 within SIG
const uint32_t EXPECTED_RVA = 0xBADD1Eu; // start of SIG in the analyzed build (cross-check only)

// ---- patch site 2: the transient "+N souls gained" popup -------------------
//   movzx eax,[esi+0x118] ; push eax ; mov ecx,edi ; call FUN_00788df0
// [esi+0x118] is the "show +N" flag; FUN_00788df0(this, flag) sets the getsoul text
// element's visible bit = flag. We overwrite the MOVZX (7 bytes) with `xor eax,eax`
// + NOP*5 so the call always receives 0 -> the popup is forced hidden every frame.
const uint8_t  SIG2[]       = { 0x0F,0xB6,0x86,0x18,0x01,0x00,0x00, 0x50, 0x8B,0xCF, 0xE8 };
const size_t   SIG2_LEN     = sizeof(SIG2);
const uint8_t  PATCH2[]     = { 0x33,0xC0, 0x90,0x90,0x90,0x90,0x90 }; // xor eax,eax; nop*5
const uint32_t EXPECTED_RVA2 = 0xBADE01u; // start of SIG2 in the analyzed build (cross-check only)

// ---- patch site 3 (HideBox): hide the ENTIRE soul display ------------------
// The dark plate is NOT a child this update function references, and NOT part of the
// transient +N popup dialog (FrpgMenuDlgFEGetSoul -- removing that dialog left the plate
// untouched). It is a generic background-sprite child of the soul dialog itself, drawn by
// the BASE dialog Draw method (widget vtable @0x011bf8f4 slot 2 = FUN_00787e90, which calls
// FUN_00786560 / FUN_00787720). Both draw paths are gated by a single byte:
//
//     if ((char)this[0x98] != '\0') { ... draw ALL child elements ... }
//
// So clearing [this+0x98] makes the soul dialog draw nothing -- plate + soul icon + number +
// popup vanish together (a truly empty bottom-right). It is per-object, so only the soul
// dialog is affected; every other dialog/menu (which shares the Menu08 plate texture) is fine.
//
// We clear it from the soul widget's own per-frame update (FUN_00fada70), where esi == this
// -- the SAME object the number guard uses (`cmp [esi+0x104],edi` at the SIG below). Reusing
// the SIG site, we overwrite the cmp+jz with:
//     C6 86 98 00 00 00 00   mov byte [esi+0x98], 0   ; clear the dialog's draw-enable
//     EB <rel>               jmp 0x00FADD99           ; the original "skip number" landing
// The jmp lands exactly where the JZ used to (the value-unchanged path the engine runs every
// frame), so flow is unchanged apart from the one field write. Cleared every frame -> the
// soul dialog never draws. Reversible; verified in-game (bottom-right empty, all other HUD and
// menus intact, no crash on soul gain). This SUPERSEDES the number/popup patches when on
// (they become redundant: a hidden dialog draws neither), so HideBox uses this site alone.

bool      g_applied = false;
mp::PatchEngine g_engine;

// ---- paths / logging (own file; mirrors PhantomUnleashed's pattern) ---------
std::string ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ModuleDir), &self);
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0'; else lstrcpyA(path, ".\\");
    return std::string(path);
}
std::string IniPath() { return ModuleDir() + "mctde-link.ini"; }
std::string LogPath() { return ModuleDir() + "HideSoulCounter.log"; }

void LogSink(const std::string& msg) {
    const std::string line = "[HideSoulCounter] " + msg;
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

// Route mp::Log (used by the PatchEngine too) to our file for the duration of this module's
// work. PhantomUnleashed runs to completion before us, so taking the sink here is safe.
void EnsureLogSink() {
    static bool once = false;
    if (!once) { DeleteFileA(LogPath().c_str()); once = true; }
    mp::SetLogSink(&LogSink);
}

// ---- bounded AoB scan over the main module's .text -------------------------
// Returns the number of matches for `sig`; writes the first match address to *out.
int ScanText(const uint8_t* sig, size_t len, uintptr_t* out) {
    *out = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) { mp::Log("scan: GetModuleHandle(NULL) returned null."); return -1; }

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { mp::Log("scan: bad DOS header."); return -1; }
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { mp::Log("scan: bad NT header."); return -1; }

    // Find the .text section.
    uintptr_t scanBegin = 0, scanEnd = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            scanBegin = base + sec->VirtualAddress;
            scanEnd   = scanBegin + sec->Misc.VirtualSize;
            break;
        }
    }
    if (!scanBegin) { mp::Log("scan: .text not found."); return -1; }

    int count = 0;
    for (uintptr_t p = scanBegin; p + len <= scanEnd; ++p) {
        const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
        if (q[0] != sig[0]) continue; // cheap reject
        if (memcmp(q, sig, len) == 0) {
            if (count == 0) *out = p;
            ++count;
        }
    }
    return count;
}

// Scan for `sig`, require a single match, log it (cross-checking expectedRva), then stage
// `patched` (verifyLen leading invariant bytes). Returns true if a patch was staged.
bool StageSite(const char* label, const uint8_t* sig, size_t sigLen,
               std::vector<uint8_t> patched, size_t verifyLen, uint32_t expectedRva,
               const char* note) {
    uintptr_t site = 0;
    const int matches = ScanText(sig, sigLen, &site);
    if (matches <= 0) {
        mp::Log(std::string(label) + ": signature not found (matches=" + std::to_string(matches)
                + "); skipping this site.");
        return false;
    }
    if (matches > 1) {
        mp::Log(std::string(label) + ": signature ambiguous (" + std::to_string(matches)
                + " matches); skipping to stay safe.");
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uint32_t  rva  = (uint32_t)(site - base);
    char b[112];
    sprintf_s(b, sizeof(b), "%s: site @ 0x%08X (rva 0x%X, expected 0x%X)%s",
              label, (unsigned)site, rva, expectedRva,
              (rva == expectedRva) ? "" : "  [rva differs -- AoB authoritative]");
    mp::Log(b);
    g_engine.Stage(site, std::move(patched), verifyLen, note);
    return true;
}

bool g_enabled = false, g_verifyOnly = true, g_hidePopup = true, g_hideBox = false, g_started = false;

} // namespace

void HideSoulCounter_Start() {
    if (g_started) return;       // idempotent
    g_started = true;
    EnsureLogSink();

    const std::string ini = IniPath();
    g_enabled    = GetPrivateProfileIntA("HideSoulCounter", "Enabled",       0, ini.c_str()) != 0;
    g_verifyOnly = GetPrivateProfileIntA("HideSoulCounter", "VerifyOnly",    1, ini.c_str()) != 0;
    g_hidePopup  = GetPrivateProfileIntA("HideSoulCounter", "HideGainPopup", 1, ini.c_str()) != 0;
    g_hideBox    = GetPrivateProfileIntA("HideSoulCounter", "HideBox",       0, ini.c_str()) != 0;

    mp::Log("----------------------------------------");
    mp::Log("Start: Enabled=" + std::to_string(g_enabled ? 1 : 0)
            + " VerifyOnly=" + std::to_string(g_verifyOnly ? 1 : 0)
            + " HideGainPopup=" + std::to_string(g_hidePopup ? 1 : 0)
            + " HideBox=" + std::to_string(g_hideBox ? 1 : 0));

    if (!g_enabled) { mp::Log("Disabled this run; leaving the HUD untouched."); return; }

    if (g_hideBox) {
        // HideBox: hide the WHOLE soul display (box + icon + number + popup) by clearing the
        // dialog's draw-enable byte [esi+0x98] every frame, at the SIG site (esi == this here).
        // 7-byte `mov byte[esi+0x98],0` then a 2-byte `jmp` to the original JZ landing. The jmp
        // rel is the original rel8 (SIG[JZ_INDEX+1]) minus 1 (our jmp sits 1 byte later than the
        // JZ did). Supersedes the number + popup patches, so this site is staged alone.
        StageSite("hide-all (box+icon+number+popup)", SIG, SIG_LEN,
                  { 0xC6,0x86,0x98,0x00,0x00,0x00,0x00, 0xEB, (uint8_t)(SIG[JZ_INDEX + 1] - 1) },
                  0, EXPECTED_RVA,
                  "mov byte[esi+0x98],0; jmp: clear soul dialog draw-enable -> whole element hidden");
    } else {
        // Site 1: persistent counter number. Flip the guard JZ to an unconditional JMP so the
        // digit text is never set (keeps CMP + rel8; verifyLen=6 confirms the invariant CMP).
        StageSite("counter-number", SIG, SIG_LEN,
                  { 0x39,0xBE,0x04,0x01,0x00,0x00, 0xEB, SIG[JZ_INDEX + 1] }, 6, EXPECTED_RVA,
                  "JZ->JMP: skip soul-counter number draw (F20 soul_param)");

        // Site 2: transient "+N souls gained" popup. Overwrite MOVZX with xor eax,eax + nop*5 so
        // the visibility call always gets 0. Opcode-changing -> verifyLen=0 (the AoB scan is the
        // authority); the unique single-match scan is what guarantees we hit the right bytes.
        if (g_hidePopup) {
            StageSite("gain-popup", SIG2, SIG2_LEN,
                      std::vector<uint8_t>(PATCH2, PATCH2 + sizeof(PATCH2)), 0, EXPECTED_RVA2,
                      "force +N getsoul popup hidden (xor eax,eax; nop*5)");
        }
    }

    if (g_engine.count() == 0) {
        mp::Log("No patch sites located; nothing to do (wrong/unpacked exe or unsupported build?).");
        return;
    }

    const int mism = g_engine.Verify();

    if (g_verifyOnly) {
        mp::Log("VerifyOnly mode: not writing. Set [HideSoulCounter] VerifyOnly=0 (and Enabled=1) "
                "once the report above shows 0 mismatches.");
        return;
    }
    if (mism != 0) {
        mp::Log("Refusing to apply: " + std::to_string(mism) + " mismatch(es).");
        return;
    }
    if (g_engine.Commit()) {
        g_applied = true;
        mp::Log(g_hideBox
                ? std::string("Applied: the ENTIRE bottom-right soul display (box + icon + number"
                              " + popup) is now hidden; rest of HUD and all menus unchanged.")
                : "Applied: soul-counter number"
                  + std::string(g_hidePopup ? " and the +N gained popup are" : " is")
                  + " now hidden (box/icon and rest of HUD unchanged).");
    } else {
        mp::Log("Apply failed; engine rolled back.");
    }
}

void HideSoulCounter_Restore() {
    if (!g_applied) return;       // idempotent: nothing committed
    EnsureLogSink();
    g_engine.RestoreAll();
    g_applied = false;
}
