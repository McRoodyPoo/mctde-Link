/*
    PhantomUnleashedStage2  -  mctde-Link (built into d3d9.dll)

    Stage 2 of the phantom-cap raise: the part that makes a >4 session actually stable.

    The players_connected_array holds its character records INLINE, so every field after
    that array shifts by 20 bytes per added slot. Stage 1's static patches resize the
    array but leave dozens of instructions reading the OLD field offsets. Stage 2 fixes
    that by:
      * 18 "pca_off" trampolines  - each replaces an instruction that touched a post-array
                                    field, recomputing the address as base + NEWBASE +
                                    pca_offset_add at runtime (pca_offset_add = 20*(N-4)).
      * 2 code-cave detours       - re-init the enlarged summon_char_types array, and fix
                                    the offsets written when players_connected_array is set up.
      * 1 static patch (patch35)  - a fixed post-array field reference.
      * 2 deferred AoB writes     - black-sign / invader level-range arrays (value (N-1)<<6);
                                    these run on a worker because the data isn't populated
                                    until later in the load sequence.

    CALIBRATION: the reverse-engineered reference is built for exactly 18 phantoms, so the
    trampoline base constants and patch35's 0xE4 are the N=18 values. pca_offset_add is
    parameterised, but patch35 is fixed -> Stage 2 is only applied when MaxPhantoms == 18.

    Stage 2 is a rewritten implementation of the MultiPhantom offset-shift trampoline
    strategy, using mctde-Link's reversible patch framework. The trampoline sites, shifted
    field offsets, and AoB patterns are derived from Metal-Crow's reverse engineering /
    Dark Souls Overhaul work. No source text is intentionally copied; the asm here (scratch-
    register choices, lea-folding, label-based branches) is our own.

    x86 / Win32 only (uses __declspec(naked) + __asm). That is mctde-Link's only target.
*/
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>

#include "PhantomUnleashedStage2.h"
#include "PatchEngine.h"   // mp::Log, mp::ThreadFreezer

namespace mp {

// ---------------------------------------------------------------------------
// Globals the naked trampolines read. file-scope statics so the inline asm in
// this same translation unit can resolve them by name.
// ---------------------------------------------------------------------------
static uint32_t g_pcaAdd = 0;          // 20 * (N - 4)
static uint32_t g_summonCount = 18;    // N (loop limit for summon_char_types re-init)
static uint32_t g_sctSuccess = 0;      // diagnostic: set to 1 by the summon cave when it runs

// indirect call / return targets, resolved at install time (base + RVA)
static uint32_t g_sctCallproc = 0, g_sctReturn = 0;
static uint32_t g_pcaCallproc = 0, g_pcaReturn = 0;

// per-trampoline return addresses (site + 5 + steal) and secondary jump targets
static uint32_t g_ret1, g_ret2, g_ret3, g_ret4, g_ret5, g_ret6, g_ret7, g_ret8, g_ret9;
static uint32_t g_ret10, g_ret11, g_ret12, g_ret13, g_ret14, g_ret15, g_ret16, g_ret17, g_ret18;
static uint32_t g_jmp2, g_jmp4, g_jmp5, g_jmp6, g_jmp16;

// ---------------------------------------------------------------------------
// Code caves (entered via an injected jmp, exited via jmp to a return address;
// never called normally, so calling convention is irrelevant).
// ---------------------------------------------------------------------------

// Re-init summon_char_types[2 .. N-1] to bl (the value the game was storing).
static void __declspec(naked) cave_summon_char_types()
{
    __asm {
        push eax
        push ecx
        mov  eax, 2
    sct_loop:
        cmp  eax, g_summonCount
        je   sct_done
        mov  ecx, edx
        add  ecx, eax
        mov  byte ptr [ecx], bl
        inc  eax
        jmp  sct_loop
    sct_done:
        pop  ecx
        pop  eax
        mov  dword ptr [g_sctSuccess], 1
        call g_sctCallproc
        jmp  g_sctReturn
    }
}

// Fix the offsets written into players_connected_array during its init. Each field
// address is base(esi) + original_offset + pca_offset_add. The mid-block `push ecx`
// is the original call argument (the callee cleans it). Returns to a FIXED address.
static void __declspec(naked) cave_pca_offsets()
{
    __asm {
        push eax
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x74]
        mov  [eax], ebx
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x88]
        mov  [eax], ebx
        pop  eax

        or   edi, -1
        push ecx                  // original call argument

        push eax
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x78]
        mov  [eax], ebx
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x7C]
        mov  [eax], edi
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x80]
        mov  [eax], edi
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x84]
        mov  [eax], edi
        pop  eax

        call g_pcaCallproc

        push eax
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x68]
        mov  [eax], edi
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x6C]
        mov  [eax], edi
        mov  eax, g_pcaAdd
        lea  eax, [eax+esi+0x70]
        mov  [eax], edi
        pop  eax

        jmp  g_pcaReturn
    }
}

// ---------------------------------------------------------------------------
// The 18 offset-shift trampolines. target = base_reg + NEWBASE + g_pcaAdd.
// ---------------------------------------------------------------------------

// cmp [eax+0xE0],esi ; setne al
static void __declspec(naked) tramp_pca1()
{
    __asm {
        push edx
        mov  edx, g_pcaAdd
        lea  edx, [edx+eax+0x7C]
        cmp  dword ptr [edx], esi
        pop  edx
        setne al
        jmp  g_ret1
    }
}

// cmp [eax+0xE0],-1 ; je <jmp2> else <ret2>
static void __declspec(naked) tramp_pca2()
{
    __asm {
        push edx
        mov  edx, g_pcaAdd
        lea  edx, [edx+eax+0x7C]
        cmp  dword ptr [edx], -1
        pop  edx
        je   take
        jmp  g_ret2
    take:
        jmp  g_jmp2
    }
}

// cmp [eax+0xE0],-1 ; setne al
static void __declspec(naked) tramp_pca3()
{
    __asm {
        push edx
        mov  edx, g_pcaAdd
        lea  edx, [edx+eax+0x7C]
        cmp  dword ptr [edx], -1
        pop  edx
        setne al
        jmp  g_ret3
    }
}

// cmp [eax+0xE0],-1 ; jne <jmp4> else <ret4>
static void __declspec(naked) tramp_pca4()
{
    __asm {
        push edx
        mov  edx, g_pcaAdd
        lea  edx, [edx+eax+0x7C]
        cmp  dword ptr [edx], -1
        pop  edx
        jne  take
        jmp  g_ret4
    take:
        jmp  g_jmp4
    }
}

// cmp [edx+0xE0],-1 ; jne <jmp5> else <ret5>   (base = edx, scratch = eax)
static void __declspec(naked) tramp_pca5()
{
    __asm {
        push eax
        mov  eax, g_pcaAdd
        lea  eax, [eax+edx+0x7C]
        cmp  dword ptr [eax], -1
        pop  eax
        jne  take
        jmp  g_ret5
    take:
        jmp  g_jmp5
    }
}

// cmp [eax+0xE0],-1 ; jne <jmp6> else <ret6>
static void __declspec(naked) tramp_pca6()
{
    __asm {
        push edx
        mov  edx, g_pcaAdd
        lea  edx, [edx+eax+0x7C]
        cmp  dword ptr [edx], -1
        pop  edx
        jne  take
        jmp  g_ret6
    take:
        jmp  g_jmp6
    }
}

// cmp [ecx+0xE0],-1 ; mov byte [esp+0x0E],0   (esp+0xE is relative to the restored esp)
static void __declspec(naked) tramp_pca7()
{
    __asm {
        push edx
        mov  edx, g_pcaAdd
        lea  edx, [edx+ecx+0x7C]
        cmp  dword ptr [edx], -1
        pop  edx
        mov  byte ptr [esp+0x0E], 0
        jmp  g_ret7
    }
}

// lea eax,[edx+0xCC] ; push esi ; xor bl,bl   (eax is the dest, so no scratch needed)
static void __declspec(naked) tramp_pca8()
{
    __asm {
        mov  eax, g_pcaAdd
        lea  eax, [eax+edx+0x68]
        push esi
        xor  bl, bl
        jmp  g_ret8
    }
}

// mov dword [edx+0xD4],0
static void __declspec(naked) tramp_pca9()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+edx+0x74]
        mov  dword ptr [ecx], 0
        pop  ecx
        jmp  g_ret9
    }
}

// inc dword [edx+0xD4] ; add eax,4
static void __declspec(naked) tramp_pca10()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+edx+0x74]
        inc  dword ptr [ecx]
        pop  ecx
        add  eax, 4
        jmp  g_ret10
    }
}

// mov edx,[ebx+0xD0] ; mov [esp+0x24],ecx   (trailing op uses the ORIGINAL ecx)
static void __declspec(naked) tramp_pca11()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+ebx+0x70]
        mov  edx, dword ptr [ecx]
        pop  ecx
        mov  dword ptr [esp+0x24], ecx
        jmp  g_ret11
    }
}

// mov edi,[eax+0xE0] ; cmp edi,-1
static void __declspec(naked) tramp_pca12()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+eax+0x7C]
        mov  edi, dword ptr [ecx]
        pop  ecx
        cmp  edi, -1
        jmp  g_ret12
    }
}

// mov edi,[eax+0xE0] ; cmp edi,-1
static void __declspec(naked) tramp_pca13()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+eax+0x7C]
        mov  edi, dword ptr [ecx]
        pop  ecx
        cmp  edi, -1
        jmp  g_ret13
    }
}

// mov eax,[esi+0xD4]
static void __declspec(naked) tramp_pca14()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+esi+0x74]
        mov  eax, dword ptr [ecx]
        pop  ecx
        jmp  g_ret14
    }
}

// mov edx,[esi+0xD8] ; cmp edx,1
static void __declspec(naked) tramp_pca15()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+esi+0x78]
        mov  edx, dword ptr [ecx]
        pop  ecx
        cmp  edx, 1
        jmp  g_ret15
    }
}

// cmp dword [edi+0xDC],2 ; jne <jmp16> else <ret16>
static void __declspec(naked) tramp_pca16()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+edi+0x78]
        cmp  dword ptr [ecx], 2
        pop  ecx
        jne  take
        jmp  g_ret16
    take:
        jmp  g_jmp16
    }
}

// mov dword [edi+0xDC],3
static void __declspec(naked) tramp_pca17()
{
    __asm {
        push ecx
        mov  ecx, g_pcaAdd
        lea  ecx, [ecx+edi+0x78]
        mov  dword ptr [ecx], 3
        pop  ecx
        jmp  g_ret17
    }
}

// mov ecx,[edi+0xE0] ; cmp ecx,-1   (op writes ecx, so scratch = eax)
static void __declspec(naked) tramp_pca18()
{
    __asm {
        push eax
        mov  eax, g_pcaAdd
        lea  eax, [eax+edi+0x7C]
        mov  ecx, dword ptr [eax]
        pop  eax
        cmp  ecx, -1
        jmp  g_ret18
    }
}

// ---------------------------------------------------------------------------
// Reversible install / restore bookkeeping.
// ---------------------------------------------------------------------------
struct S2Saved { uintptr_t addr; std::vector<uint8_t> orig; };
static std::vector<S2Saved> g_saved;
static CRITICAL_SECTION     g_savedLock;
static bool                 g_savedLockReady = false;
static bool                 g_installed = false;
static HANDLE               g_aobThread = nullptr;
static volatile LONG        g_aobStop = 0;

static void EnsureLock()
{
    if (!g_savedLockReady) { InitializeCriticalSection(&g_savedLock); g_savedLockReady = true; }
}

static void WriteBytes(uintptr_t addr, const uint8_t* data, size_t len)
{
    DWORD oldProt = 0;
    if (VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy((void*)addr, data, len);
        DWORD tmp = 0;
        VirtualProtect((void*)addr, len, oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    }
}

// Capture `len` original bytes for restore, then write `data`.
static void SaveAndWrite(uintptr_t addr, const uint8_t* data, size_t len)
{
    S2Saved s;
    s.addr = addr;
    s.orig.assign((const uint8_t*)addr, (const uint8_t*)addr + len);
    EnterCriticalSection(&g_savedLock);
    g_saved.push_back(std::move(s));
    LeaveCriticalSection(&g_savedLock);
    WriteBytes(addr, data, len);
}

// Inject a 5-byte E9 jmp (+ nop padding) at `site`, capturing the overwritten bytes.
// Sets *retVar = site + 5 + stealExtra (where the trampoline jumps back), unless null.
static void InjectJmp(uintptr_t site, int stealExtra, void* target, uint32_t* retVar)
{
    const size_t len = 5 + (size_t)stealExtra;
    std::vector<uint8_t> buf(len, 0x90); // nop fill
    const int32_t rel = (int32_t)((uintptr_t)target - (site + 5));
    buf[0] = 0xE9;
    memcpy(&buf[1], &rel, 4);
    SaveAndWrite(site, buf.data(), len);
    if (retVar) *retVar = (uint32_t)(site + len);
}

// ---- deferred AoB pass (worker thread) ------------------------------------

static bool ParseImage(uintptr_t base, const uint8_t** begin, size_t* size)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const DWORD img = nt->OptionalHeader.SizeOfImage;
    if (img == 0 || img > 0x04000000) return false;
    *begin = (const uint8_t*)base;
    *size  = img;
    return true;
}

// Exact-byte scan over the loaded image. Returns address of first match or 0.
static uintptr_t AobScan(uintptr_t base, const uint8_t* pat, size_t patLen)
{
    const uint8_t* img = nullptr; size_t size = 0;
    if (!ParseImage(base, &img, &size)) return 0;
    for (size_t i = 0; i + patLen <= size; ++i) {
        // Skip pages that aren't readable to avoid faulting on guard regions.
        if ((i & 0xFFF) == 0) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(img + i, &mbi, sizeof(mbi)) &&
                (mbi.State != MEM_COMMIT ||
                 (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) {
                i += 0xFFF; // jump to next page boundary-ish
                continue;
            }
        }
        size_t j = 0;
        for (; j < patLen; ++j) if (img[i + j] != pat[j]) break;
        if (j == patLen) return (uintptr_t)(img + i);
    }
    return 0;
}

static const uint8_t kBlackSos[28] = {
    0x05,0,0,0, 0x03,0,0,0, 0x03,0,0,0, 0x05,0,0,0,
    0x05,0,0,0, 0x0A,0,0,0, 0x0A,0,0,0
};
static const uint8_t kInvade[28] = {
    0x05,0,0,0, 0xFA,0xFF,0xFF,0xFF, 0x08,0,0,0, 0xFC,0xFF,0xFF,0xFF,
    0x0A,0,0,0, 0xFE,0xFF,0xFF,0xFF, 0x0C,0,0,0
};

// Read a u32 only if the page is committed & readable, so a stale/null pointer can't fault.
static bool TryReadU32(uintptr_t addr, uint32_t* out)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    *out = *(const volatile uint32_t*)addr;
    return true;
}

static inline bool LooksLikePtr(uint32_t v) { return v >= 0x10000 && v < 0x80000000; }

// "Set maximum number of phantoms": follow [base + 0xF7D82C] -> +0x08C and write N. This is
// the LIVE session field the host checks when accepting a summon -- without it the summon
// banner fires but the 5th player never seats. The struct isn't allocated until the player
// is online and can reset per session, so we (re)assert it on a gentle poll. It's volatile
// session data (re-derived by the game each session), so it is intentionally NOT restore-tracked.
static void EnforceMaxSummons(uintptr_t base, uint32_t n, bool* loggedOnce)
{
    uint32_t ptr = 0;
    if (!TryReadU32(base + 0xF7D82C, &ptr) || !LooksLikePtr(ptr)) return;
    const uintptr_t target = (uintptr_t)ptr + 0x08C;
    uint32_t cur = 0;
    if (TryReadU32(target, &cur) && cur == n) return; // already correct; nothing to do
    WriteBytes(target, (const uint8_t*)&n, 4);
    if (loggedOnce && !*loggedOnce) {
        mp::Log("Stage2: live max-summons field set to " + std::to_string(n)
                + " (host can now seat >4).");
        *loggedOnce = true;
    }
}

static DWORD WINAPI AobThread(LPVOID param)
{
    const uintptr_t base = (uintptr_t)param;
    const uint32_t value = (uint32_t)(g_summonCount - 1) << 6; // (N-1)<<6
    const uint32_t maxSummons = g_summonCount;                 // N

    bool blackDone = false, invadeDone = false, maxLogged = false, aobGaveUp = false;
    int aobAttempts = 0;
    const int kAobMax = 240; // ~2 min of level-range scanning; bounded

    // Runs for the life of the process (exits on teardown via g_aobStop). The AoB level-range
    // writes are one-shot/bounded; the max-summons field is re-asserted every tick because the
    // session can come up late or reset.
    while (!g_aobStop) {
        if (!blackDone && aobAttempts < kAobMax) {
            uintptr_t a = AobScan(base, kBlackSos, sizeof(kBlackSos));
            if (a) { SaveAndWrite(a, (const uint8_t*)&value, 4); blackDone = true;
                     mp::Log("Stage2 AoB: BlackSos level-range patched."); }
        }
        if (!invadeDone && aobAttempts < kAobMax) {
            uintptr_t a = AobScan(base, kInvade, sizeof(kInvade));
            if (a) { SaveAndWrite(a, (const uint8_t*)&value, 4); invadeDone = true;
                     mp::Log("Stage2 AoB: Invade level-range patched."); }
        }
        if (!aobGaveUp && aobAttempts >= kAobMax && (!blackDone || !invadeDone)) {
            mp::Log("Stage2 AoB: gave up after bound (black=" + std::to_string(blackDone)
                    + " invade=" + std::to_string(invadeDone) + "). Level-range counts unchanged.");
            aobGaveUp = true;
        }
        ++aobAttempts;

        EnforceMaxSummons(base, maxSummons, &maxLogged);

        Sleep(500);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void PhantomUnleashedStage2_Install(uint8_t N)
{
    EnsureLock();
    if (g_installed) { mp::Log("Stage2: already installed."); return; }

    if (N != 18) {
        mp::Log("Stage2: MaxPhantoms=" + std::to_string((int)N)
                + " but the offset trampolines are calibrated for 18. SKIPPING Stage 2 "
                  "(a >4 session will not be stable at this N).");
        return;
    }

    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!base) { mp::Log("Stage2: GetModuleHandle(NULL) null; aborting."); return; }

    g_pcaAdd      = 20u * (uint32_t)(N - 4); // 0x118 at N=18
    g_summonCount = N;
    g_sctSuccess  = 0;

    // Freeze other threads while we rewrite live code.
    mp::ThreadFreezer freeze;

    // --- code caves ---
    g_sctCallproc = (uint32_t)(base + 0x1629C0);
    InjectJmp(base + 0x8068DF, 3, (void*)&cave_summon_char_types, &g_sctReturn);

    g_pcaCallproc = (uint32_t)(base + 0xAA2320);
    g_pcaReturn   = (uint32_t)(base + 0xAA24FF); // explicit, not site+len
    InjectJmp(base + 0xAA24D2, 4, (void*)&cave_pca_offsets, nullptr);

    // --- 18 offset-shift trampolines ---
    InjectJmp(base + 0xA6615A, 1, (void*)&tramp_pca1,  &g_ret1);
    g_jmp2 = (uint32_t)(base + 0xA5B939);
    InjectJmp(base + 0xA5B900, 1, (void*)&tramp_pca2,  &g_ret2);
    InjectJmp(base + 0x9C83DB, 2, (void*)&tramp_pca3,  &g_ret3);
    g_jmp4 = (uint32_t)(base + 0x9C6BDB);
    InjectJmp(base + 0x9C6BBE, 1, (void*)&tramp_pca4,  &g_ret4);
    g_jmp5 = (uint32_t)(base + 0x92A453);
    InjectJmp(base + 0x92A44A, 1, (void*)&tramp_pca5,  &g_ret5);
    g_jmp6 = (uint32_t)(base + 0x9B5405);
    InjectJmp(base + 0x9B53FB, 1, (void*)&tramp_pca6,  &g_ret6);
    InjectJmp(base + 0x93FEEE, 4, (void*)&tramp_pca7,  &g_ret7);
    InjectJmp(base + 0xAA1771, 1, (void*)&tramp_pca8,  &g_ret8);
    InjectJmp(base + 0xAA1791, 2, (void*)&tramp_pca9,  &g_ret9);
    InjectJmp(base + 0xAA17AD, 1, (void*)&tramp_pca10, &g_ret10);
    InjectJmp(base + 0xAA29A9, 2, (void*)&tramp_pca11, &g_ret11);
    InjectJmp(base + 0xAA3141, 1, (void*)&tramp_pca12, &g_ret12);
    InjectJmp(base + 0xAA3268, 1, (void*)&tramp_pca13, &g_ret13);
    InjectJmp(base + 0xAA18E2, 0, (void*)&tramp_pca14, &g_ret14);
    InjectJmp(base + 0xAA18E7, 1, (void*)&tramp_pca15, &g_ret15);
    g_jmp16 = (uint32_t)(base + 0xAA27A9);
    InjectJmp(base + 0xAA2748, 1, (void*)&tramp_pca16, &g_ret16);
    InjectJmp(base + 0xAA27A1, 2, (void*)&tramp_pca17, &g_ret17);
    InjectJmp(base + 0xAA2740, 1, (void*)&tramp_pca18, &g_ret18);

    // --- static patch35: mov ecx,[edi+0xE4] (fixed post-array field, N=18 value) ---
    {
        const uint8_t patch35[6] = { 0x8B, 0x8F, 0xE4, 0x00, 0x00, 0x00 };
        SaveAndWrite(base + 0xAA277A, patch35, sizeof(patch35));
    }

    g_installed = true;
    mp::Log("Stage2: installed 2 caves + 18 trampolines + patch35 ("
            + std::to_string(g_saved.size()) + " sites). pca_offset_add=0x"
            + [&]{ char b[16]; sprintf_s(b, sizeof(b), "%X", g_pcaAdd); return std::string(b); }());

    // Defer the level-range AoB writes: the data isn't populated until later in load.
    g_aobStop = 0;
    g_aobThread = CreateThread(nullptr, 0, AobThread, (LPVOID)base, 0, nullptr);
    if (!g_aobThread) mp::Log("Stage2: failed to start AoB worker thread.");
}

void PhantomUnleashedStage2_Restore()
{
    if (!g_savedLockReady) return;

    // Stop the AoB worker before we revert (it may still be writing).
    InterlockedExchange(&g_aobStop, 1);
    if (g_aobThread) {
        WaitForSingleObject(g_aobThread, 2000);
        CloseHandle(g_aobThread);
        g_aobThread = nullptr;
    }

    mp::ThreadFreezer freeze;

    EnterCriticalSection(&g_savedLock);
    size_t n = 0;
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
        WriteBytes(it->addr, it->orig.data(), it->orig.size());
        ++n;
    }
    g_saved.clear();
    LeaveCriticalSection(&g_savedLock);

    g_installed = false;
    mp::Log("Stage2: reverted " + std::to_string(n) + " site(s).");
}

} // namespace mp
