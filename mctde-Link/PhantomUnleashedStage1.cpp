/*
    PhantomUnleashedStage1  -  mctde-Link (built into d3d9.dll)
    Stage 1: static, count-parameterized byte patches.

    Based on Metal-Crow's reverse-engineered MultiPhantom / Dark Souls Overhaul patch map
    and behavior. This file rewrites the implementation for mctde-Link's patch engine with
    parameterization, verification, logging, and reversible restore. No source text is
    intentionally copied; the RVAs/opcodes/offset facts are credited to the original
    reverse-engineering work.

    Each Stage() call below records:
        base + RVA   -> the patch site (a fact about the PTDE binary)
        { bytes }    -> the instruction with our phantom count substituted in
        verifyLen    -> how many leading opcode bytes are invariant (used by Verify
                        to confirm the offset points at the expected instruction)
        note         -> what the instruction does / why it must change

    The opcode/modrm bytes are identical before and after patching; only the trailing
    immediate (the phantom count, an allocation size, or a bound) changes. That is why
    Verify can compare the live bytes against the leading bytes of our patched form.
*/
#include "PhantomUnleashedStage1.h"
#include "PatchEngine.h"

#include <cstdio>

namespace mp {

// ---- the single source of truth for "how many phantoms" --------------------
static uint8_t  N    = 18;                // total slots
static uint8_t  VER  = 0x4D;              // matchmaking pool key (vanilla = 0x2E)
static uintptr_t g_base = 0;
static PatchEngine g_engine;

// little-endian byte extract
static inline uint8_t LE(uint32_t v, int i) { return (uint8_t)((v >> (8 * i)) & 0xFF); }
static inline std::vector<uint8_t> dword(uint32_t v) { return { LE(v,0), LE(v,1), LE(v,2), LE(v,3) }; }

// derived sizes (facts: layout of the summon / connected-player structures)
static inline uint32_t SummonDataAlloc()   { return 16u + (uint32_t)N * 1216u; }   // summon_chars_data block
static inline uint32_t SummonDataBytes()   { return (uint32_t)N * 1216u; }          // byte length (not count)
static inline uint32_t ConnectedAlloc()    { return 24u + (uint32_t)N * 20u + 36u; }// players_connected_array block

// convenience: stage a patch at base + rva
static void P(uintptr_t rva, std::vector<uint8_t> bytes, size_t verifyLen, const char* note) {
    g_engine.Stage(g_base + rva, std::move(bytes), verifyLen, note);
}

bool PhantomUnleashedStage1_Prepare(const PhantomUnleashedConfig& cfg) {
    if (cfg.maxPhantoms < 4 || cfg.maxPhantoms > 32) {
        Log("Prepare: maxPhantoms must be in [4, 32].");
        return false;
    }
    N   = cfg.maxPhantoms;
    VER = cfg.networkVersion;

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!g_base) { Log("Prepare: GetModuleHandle(NULL) returned null."); return false; }

    const uint8_t n   = N;
    const uint8_t nm1 = (uint8_t)(N - 1);
    const uint8_t ver = VER;

    Log("Prepare: ds1_base = 0x" + std::to_string((unsigned)g_base)
        + ", N = " + std::to_string((int)N)
        + ", NetVer = 0x" + [&]{ char b[8]; sprintf_s(b, sizeof(b), "%02X", VER); return std::string(b); }()
        + (VER == 0x2E ? " (vanilla pool -- NO segregation)" : " (segregated pool)"));

    // -- session / summon allocation sizes -----------------------------------
    P(0xA63CA6, [&]{ auto b = dword((uint32_t)n * 0x20u); b.insert(b.begin(), 0x68); return b; }(), 1,
      "push N*0x20 (summon slot stride)");
    P(0xA63CBA, [&]{ auto b = dword((uint32_t)n * 0x20u); b.insert(b.begin(), 0x68); return b; }(), 1,
      "push N*0x20 (summon slot stride, 2nd)");
    P(0xA63CC1, { 0xC7, 0x07, n, 0x00, 0x00, 0x00 }, 2, "mov [edi], N (number of char slots)");

    // -- sign visibility caps -------------------------------------------------
    P(0x9B51B7, { 0x6A, n }, 1, "push N (max summons before signs vanish, white)");
    P(0x9B51EB, { 0x6A, n }, 1, "push N (max summons before signs vanish, red)");

    // -- aggregate phantom check (the *7 is Metal-Crow's empirical factor) ----
    P(0x9B11BE, { 0x83, 0xFA, (uint8_t)(n * 7) }, 2, "cmp edx, N*7 (sums all phantoms)");

    // -- assorted count comparisons (purpose partly uncertain upstream) -------
    P(0x92A47C, { 0x83, 0x78, 0x10, n }, 3, "cmp [eax+0x10], N");
    P(0x92A465, { 0x83, 0xB8, 0x90, 0x00, 0x00, 0x00, n }, 6, "cmp [eax+0x90], N");
    // Opcode-CHANGING patch: the original here is `3B 86 <disp32>` (cmp eax,[esi+disp32],
    // 6 bytes); we replace the whole instruction with `cmp eax, N` + 3 nops. Because the
    // leading opcode is not invariant, this site can't be leading-byte verified (verifyLen=0).
    P(0x9B1271, { 0x83, 0xF8, n, 0x90, 0x90, 0x90 }, 0, "cmp eax, N; nop*3 (sign display; was cmp eax,[esi+x])");

    // -- summon_chars_data array ---------------------------------------------
    P(0x806848, [&]{ auto b = dword(SummonDataAlloc()); b.insert(b.begin(), 0x68); return b; }(), 1,
      "push 16+1216*N (summon_chars_data alloc)");
    P(0x8068A4, { 0x83, 0xFE, n }, 2, "cmp esi, N (init loop limit)");
    P(0x806ADE, [&]{ auto b = dword(SummonDataBytes()); b.insert(b.begin(), { 0x81, 0xFB }); return b; }(), 2,
      "cmp ebx, 1216*N (byte offset, not index)");
    P(0x8068A9, { 0xC7, 0x41, 0xFC, n, 0x00, 0x00, 0x00 }, 3, "mov [ecx-4], N (stored array length)");

    // -- bounds checks --------------------------------------------------------
    P(0x8037A7, { 0x83, 0xFA, n }, 2, "cmp edx, N (bounds)");
    P(0x8037BF, [&]{ auto b = dword(SummonDataBytes()); b.insert(b.begin(), { 0x81, 0xF9 }); return b; }(), 2,
      "cmp ecx, 1216*N (bounds, bytes)");
    P(0x802CC0, { 0x83, 0xF8, n }, 2, "cmp eax, N (bounds)");

    // -- summon_char_types array size ----------------------------------------
    P(0x8068C3, { 0x6A, n }, 1, "push N (summon_char_types size)");

    // -- summon_char_types length limits (these compare against N-1 / <=) -----
    P(0x802CB7, { 0x83, 0xF8, n   }, 2, "cmp eax, N");
    P(0x802C80, { 0x83, 0xF9, nm1 }, 2, "cmp ecx, N-1");
    P(0x92E7A9, { 0x83, 0xFB, nm1 }, 2, "cmp ebx, N-1");
    P(0x92E6AF, { 0x83, 0xFB, nm1 }, 2, "cmp ebx, N-1");
    P(0x92FB73, { 0x83, 0xFE, nm1 }, 2, "cmp esi, N-1");
    P(0x92FDF1, { 0x83, 0xFF, nm1 }, 2, "cmp edi, N-1");
    P(0x930431, { 0x83, 0xF8, nm1 }, 2, "cmp eax, N-1");
    P(0x931C66, { 0x83, 0xFE, nm1 }, 2, "cmp esi, N-1");
    P(0x928135, { 0x83, 0xF8, nm1 }, 2, "cmp eax, N-1");
    P(0x92B19A, { 0x83, 0xFF, nm1 }, 2, "cmp edi, N-1");
    P(0xAE2B8A, { 0x83, 0xF8, nm1 }, 2, "cmp eax, N-1");

    // -- char-pointer array init size ----------------------------------------
    P(0x322A00, { 0x6A, n }, 1, "push N (char pointer array size)");

    // -- matchmaking pool key (POOL SEGREGATION) ------------------------------
    // The game pairs players only when this 1-byte network version matches. Vanilla
    // retail = 0x2E. Writing a PhantomUnleashed value here puts you in a pool with ONLY
    // other PhantomUnleashed players who share the same NetworkVersion. WITHOUT these three
    // patches the cap-raise alone leaves you in the vanilla pool (the bug we just hit).
    // The leading opcode bytes are invariant; only the version immediate changes.
    P(0x7E73FA, { 0xC6, 0x44, 0x24, 0x1C, ver }, 4, "mov [esp+0x1C], NetVer (advertise version)");
    P(0x7E719D, { 0x80, 0x38, ver }, 2, "cmp [eax], NetVer (peer version check 1)");
    P(0x7E7229, { 0x80, 0x38, ver }, 2, "cmp [eax], NetVer (peer version check 2)");

    // -- connection capacity --------------------------------------------------
    // Max connection tickets: a packed dword whose high byte is the count. The reference
    // pins this at a FIXED 0x08000000 (= 8 in the high byte) even at N=18 -- it is NOT
    // N<<24 (that over-counts and was a bug here). Match the proven value.
    P(0xEDE2CC, dword(0x08000000u), 0, "max connection tickets = 0x08000000 (fixed)");
    // Lobby access max count: mov [esi+0xD4], N-1
    P(0x7E9AD1, { 0xC7, 0x86, 0xD4, 0x00, 0x00, 0x00, nm1 }, 6, "mov [esi+0xD4], N-1 (lobby max)");

    // -- game internal memory pool -------------------------------------------
    // Stock is `push 0xA20000` (~10 MB) at three sites -- sized for 4 players, too small
    // once many phantoms load their models/gear/animations. Raise it so an N=18 session has
    // room. Clamp to the safe max 0x0FFFFFFF (>= 0x1FFFFFFF crashes). 0 leaves it stock.
    if (cfg.memoryPoolBytes > 0xA20000u) {
        uint32_t pool = cfg.memoryPoolBytes;
        if (pool > 0x0FFFFFFFu) pool = 0x0FFFFFFFu;
        P(0xB8B7E5, [&]{ auto b = dword(pool); b.insert(b.begin(), 0x68); return b; }(), 1, "push pool size (game memory pool 1)");
        P(0x9E20,   [&]{ auto b = dword(pool); b.insert(b.begin(), 0x68); return b; }(), 1, "push pool size (game memory pool 2)");
        P(0x9E41,   [&]{ auto b = dword(pool); b.insert(b.begin(), 0x68); return b; }(), 1, "push pool size (game memory pool 3)");
        Log("Prepare: raising game memory pool to "
            + [&]{ char b[16]; sprintf_s(b, sizeof(b), "0x%X", pool); return std::string(b); }()
            + " (" + std::to_string(pool / (1024u * 1024u)) + " MB).");
    }

    // -- players_connected_array ---------------------------------------------
    P(0xAA24AA, { 0x6A, n }, 1, "push N (connected array init count)");
    P(0xABB55D, [&]{ auto b = dword(ConnectedAlloc()); b.insert(b.begin(), 0x68); return b; }(), 1,
      "push 24+20*N+36 (players_connected_array alloc)");

    // -- per-function slot counts iterated over players_connected_array -------
    P(0xAA1A86, { 0xC7, 0x44, 0x24, 0x10, n, 0x00, 0x00 }, 4, "mov [esp+0x10], N");
    P(0xAA2FDE, { 0xC7, 0x44, 0x24, 0x14, n, 0x00, 0x00 }, 4, "mov [esp+0x14], N");
    P(0xAA2554, { 0x83, 0xFA, n }, 2, "cmp edx, N");
    P(0xAA2569, { 0x83, 0xFB, n }, 2, "cmp ebx, N");
    P(0xC12C71, { 0x83, 0xFE, n }, 2, "cmp esi, N");
    P(0xAA2932, { 0xC7, 0x44, 0x24, 0x14, n, 0x00, 0x00 }, 4, "mov [esp+0x14], N");
    P(0xAA2C8E, { 0xC7, 0x44, 0x24, 0x14, n, 0x00, 0x00 }, 4, "mov [esp+0x14], N");
    P(0xAA2A2C, { 0x83, 0xF8, n }, 2, "cmp eax, N");
    P(0xAA2C06, { 0x83, 0xFF, n }, 2, "cmp edi, N");
    P(0xAA2C18, { 0x83, 0xF8, n }, 2, "cmp eax, N");

    // -- phantom-slot counts (N-1) over the same array ------------------------
    P(0xAA1788, { 0x83, 0xF9, nm1 }, 2, "cmp ecx, N-1 (phantom slots)");
    P(0xAA1798, [&]{ auto b = dword((uint32_t)nm1); b.insert(b.begin(), 0xB9); return b; }(), 1,
      "mov ecx, N-1");
    P(0xAA2BFF, { 0x8D, 0x5F, nm1 }, 2, "lea ebx, [edi + (N-1)]");

    Log("Prepare: staged " + std::to_string(g_engine.count()) + " static patch(es).");
    return true;
}

int  PhantomUnleashedStage1_Verify()  { return g_engine.Verify(); }
bool PhantomUnleashedStage1_Apply()   { return g_engine.Commit(); }
void PhantomUnleashedStage1_Restore() { g_engine.RestoreAll(); }

} // namespace mp
