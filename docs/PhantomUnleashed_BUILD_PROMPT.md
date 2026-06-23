# PhantomUnleashed — Implementation Brief / Build Prompt

> Hand this whole file to a fresh Claude Code session (or use it yourself) to continue the
> PhantomUnleashed feature. It is self-contained: goal, current state, requirements, file paths,
> the offset facts, the licensing guardrail, and a test plan. Read the "Licensing guardrail"
> and "What this is / is NOT" sections before writing any code.

---

## 1. Goal (one sentence)

Add a **PhantomUnleashed** feature to **mctde-Link** that raises Dark Souls PTDE's simultaneous-phantom
cap above the stock 4, **built directly into mctde-Link's `d3d9.dll`** (NOT a separate DLL),
gated by a **launch-time Yes/No prompt**, with a clean, reversible, crash-safe patch engine.

---

## 2. What this is / is NOT (read this)

- This is a **derivative rewrite, with attribution** — not a clean-room effort. It re-implements
  Metal-Crow's MultiPhantom / Dark Souls Overhaul phantom-limit patch (the patch *sites*, offset
  facts, trampoline strategy, and AoB patterns come from his reverse engineering) on top of
  mctde-Link's own patch engine.
- **Do NOT copy source text** from the reference verbatim — write our own framework, naming,
  structure, and asm. But be honest that the *design and behavior are derived*; credit Metal-Crow
  prominently in file headers and the README.
- The reference checkout's `Inferno` branch has **no LICENSE file** (its `master` is AGPLv3). Treat
  the checkout as read-only reference, and since this work is derivative of it, keep licensing
  compatible / defer to the upstream author's terms rather than asserting an unencumbered license.
- Credit Metal-Crow for the reverse engineering in file headers and README (done).

---

## 3. Current state (already done — do not redo)

Stage 1 exists as a **separate project** at `mctde-PhantomUnleashed/` (this needs to be folded into
mctde-Link per Requirement A). It contains:

- `mctde-PhantomUnleashed/PatchEngine.h` / `.cpp` — our own reversible patch framework:
  - saves original bytes, applies with **all other threads frozen**, restores on unload,
  - `Verify()` dry-run that confirms each site's leading opcode bytes match (catches wrong offsets),
  - never spins forever.
- `mctde-PhantomUnleashed/PhantomLimit.h` / `.cpp` — the **Stage 1 static patch table**: ~50
  count-parameterized byte patches, fully documented, keyed off `N = MaxPhantoms`.
- `mctde-PhantomUnleashed/dllmain.cpp` — separate-DLL plugin entry (to be REPLACED by a built-in
  `PhantomUnleashed_Start()` — see Requirement A).
- `mctde-PhantomUnleashed/mctde-PhantomUnleashed.vcxproj` — separate project (to be DELETED after folding in).

Namespace is `mp::`. Stage 1 alone is **not a working multiphantom** — it needs Stage 2 (Requirement E).

---

## 4. Requirements

### A. Build it INTO mctde-Link (remove the separate DLL)
- Move `PatchEngine.{h,cpp}` and `PhantomLimit.{h,cpp}` into the `mctde-Link/` source folder and add
  them to `mctde-Link/mctde-Link.vcxproj` (+ `.vcxproj.filters`).
- Replace the separate `dllmain.cpp` with a single entry, e.g. `void PhantomUnleashed_Start();`, called from
  `StartBuiltInModules()` in `mctde-Link/dllmain.cpp:556` (alongside the NetOverlay/VersionChecker starts).
- Delete `mctde-PhantomUnleashed/` and its `.vcxproj`. Remove it from `mctde-Link.sln` if added.
- Use mctde-Link's existing logging (`WriteLogLine`/`WriteLogf` in `MCTDE_NetOverlay.cpp`) via a small
  shim, or keep `mp::SetLogSink()` pointed at it. Reuse the existing ini path/helpers.
- **Auto-loads inherently** once built in — this is the whole point of folding it in (the user observed
  the old `phantom_break.dll` auto-loads from DATA via some external loader; building in removes any such
  dependency). mctde-Link itself does NOT currently load `phantom_break.dll` (verified: its only mod-DLL
  load is `LoadOneDll` at `dllmain.cpp:377`, fed by ini `GenericDLL=` + the `ChainloadFolder` scan).

### B. Launch-time Yes/No prompt (NEW — core requirement)
- **Before the game opens**, prompt the player: *"Enable PhantomUnleashed? (You will only be able to connect
  with other players who also have PhantomUnleashed enabled.)"* — Yes / No.
- Mechanism: show a **modal `MessageBox`** (MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND,
  owner `NULL`) on **mctde-Link's `HubThread`** (`dllmain.cpp:575`), early — before `StartBuiltInModules()`
  applies anything and before any online/session init. Store the answer for the session.
  - MessageBox on HubThread (a real thread, not DllMain) avoids loader-lock issues.
- **If No:** do nothing — no patches applied, no pool segregation, vanilla matchmaking. The player can
  connect with everyone normally.
- **If Yes:** apply the PhantomUnleashed patch set → player is in the segregated pool (see Requirement C).
- ini control in `[PhantomUnleashed]`:
  - `Mode = Ask | On | Off` (default **Ask** = show the prompt every launch).
    - `On` = auto-enable, skip prompt. `Off` = disabled, skip prompt.
  - Keep a dev/diagnostic `VerifyOnly = 1` flag (default 1 during bring-up) that makes the engine only
    run `Verify()` and log, never write — independent of the Yes/No gate.

### C. Player-pool segregation (verify + make clean)
- Enabling PhantomUnleashed must put the player in a **separate matchmaking pool**: only other players with
  PhantomUnleashed enabled can connect. Disabling leaves the player in the normal pool.
- This segregation is expected to fall out of the connection/lobby patches (`max connection tickets`,
  `lobby access max count`) — **but verify it actually segregates cleanly.** A vanilla (un-modded) player
  must NEVER be pulled into a >4 session (that risks desync/crash for them).
- If the raw cap-patches do not cleanly segregate, add an explicit pool key (e.g. a matchmaking
  password / session-version tag) so PhantomUnleashed users only ever see each other. **Open question —
  confirm the mechanism against the reference before shipping (see §8).**

### D. Safety (carry these through Stage 2)
- Save originals; **freeze all other threads** during any code write (`ThreadFreezer` already does this).
- **No unbounded scans.** The reference uses `while(scan==NULL){Sleep(100);}` — replace with a bounded
  retry (N attempts, then fail loudly). Reuse mctde-Link's `FindPatternInExe`
  (`MCTDE_NetOverlay.cpp:1274`) for AoB.
- `Verify()` must pass (0 mismatches) before `Commit()`; refuse to apply on mismatch.
- **Restore on teardown.** Call `PhantomLimit_Restore()` from `McTDE_NetOverlay_OnProcessDetach`
  (`MCTDE_NetOverlay.cpp:6882`) / `DLL_PROCESS_DETACH` (`dllmain.cpp:688`), AND tie it into the existing
  **WatchdogThread** (`MCTDE_NetOverlay.cpp:6744`, `ExitProcess` at 6789) so a window-gone teardown also
  reverts. (Memory: `zombie-process-watchdog`.)

### E. Stage 2 — the part that makes >4 actually work (TODO)
The stock player record array is **inline** inside a struct, so every field after it shifts by `0x20`
per added slot. Stage 1's static patches are not enough; you must add:
1. **18 offset-shift trampolines** (`pca_off1..18` in the reference): each hooks a site that reads a
   post-array field and recomputes the offset as `20*(N-4) + original_offset` at runtime.
2. **2 code-cave detours:**
   - `summon_char_types_newmem` — re-init loop for the enlarged `summon_char_types` array (hook at
     `ds1_base + 0x8068DF`, 3-byte steal; calls back to `ds1_base + 0x1629C0`).
   - `players_connected_array_offsets` — fix offsets after the enlarged inline array on init
     (hook at `ds1_base + 0xAA24D2`, 4-byte steal; callproc `ds1_base + 0xAA2320`,
     return `ds1_base + 0xAA24FF`).
   - plus static `patch35`: `mov ecx,[edi+0xE4]` at `ds1_base + 0xAA277A` (fixed bytes
     `8B 8F E4 00 00 00`) — part of the pca offset-reference fixes.
3. **2 bounded AoB scans** for the level-range arrays (purpose: black-sign / invader counts):
   - BlackSos: `05 00 00 00 03 00 00 00 03 00 00 00 05 00 00 00 05 00 00 00 0A 00 00 00 0A 00 00 00`
     → write `(uint32_t)((N-1) << 6)` (per the dynamic reference) or `0x08000000` (per the static one) —
     **confirm which against the reference and your N.**
   - Invade: `05 00 00 00 FA FF FF FF 08 00 00 00 FC FF FF FF 0A 00 00 00 FE FF FF FF 0C 00 00 00`
     → same value treatment.
- **Author your own trampolines** (your own register/scratch choices, your own cave allocation that
  supports restore). Do not paste the reference's naked-asm verbatim. The *operation* per site is a fact;
  the exact asm text is the gray area — write it yourself. Make the trampolines reversible (track caves so
  `RestoreAll` frees/repoints them).

### F. Config / ini
Add a `[PhantomUnleashed]` section to `samples/mctde-link.sample.ini` and document it:
```ini
[PhantomUnleashed]
Mode        = Ask    ; Ask = prompt each launch (default) | On = always | Off = never
MaxPhantoms = 6      ; total player slots in your world (stock 4; reference goes to 18)
VerifyOnly  = 1      ; dev: 1 = only log the offset check, never patch. Set 0 to actually apply.
```

### G. Docs
Update `README.md`: replace/append the "Phantom Break Compatibility" section to describe the built-in
PhantomUnleashed feature, the Yes/No prompt, the segregated pool, and the ini keys. Keep the Metal-Crow RE
credit (README already references the Overhaul repo).

---

## 5. Key files (paths)

### Our module (current — to be folded into mctde-Link)
- `mctde-PhantomUnleashed/PatchEngine.h`
- `mctde-PhantomUnleashed/PatchEngine.cpp`
- `mctde-PhantomUnleashed/PhantomLimit.h`
- `mctde-PhantomUnleashed/PhantomLimit.cpp`  ← full Stage 1 static patch table lives here
- `mctde-PhantomUnleashed/dllmain.cpp`        ← replace with built-in `PhantomUnleashed_Start()`
- `mctde-PhantomUnleashed/mctde-PhantomUnleashed.vcxproj`  ← delete after folding in

### mctde-Link host integration points
- `mctde-Link/dllmain.cpp`
  - `HubThread` (575) — put the launch-time prompt here, early
  - `StartBuiltInModules` (556) — call `PhantomUnleashed_Start()` here
  - host exports `get_game_window` / `print_console` (197–232)
  - `DllMain` attach/detach (660 / 688) — restore on detach
  - `LoadOneDll` (377) — the only mod-DLL loader (for reference; not used once built in)
- `mctde-Link/MCTDE_NetOverlay.cpp`
  - `FindPatternInExe` (1274) — bounded AoB helper to reuse
  - module base `GetModuleHandleA(NULL)` (462) — this is `ds1_base`
  - ini read pattern (e.g. 997, 1042) and `g_iniPath`
  - `WatchdogThread` (6744; `ExitProcess` 6789) — hook restore in
  - `McTDE_NetOverlay_OnProcessAttach/Detach` (6877 / 6882)
- `mctde-Link/D3DOverlay.cpp` / `.h` — device hook install (`D3DOverlay_InstallEarly`), context
- `mctde-Link/mctde-Link.vcxproj` (+ `.vcxproj.filters`) — add the moved source files (Win32 / v143)
- `mctde-Link.sln`
- `samples/mctde-link.sample.ini` — add `[PhantomUnleashed]`
- `README.md`

### Read-only FACTS reference (do NOT copy source text)
- `C:\Users\treyf\Downloads\_dso_current` — Overhaul repo, currently on the **Inferno** branch (x86 PTDE):
  - `OverhaulDLL/src/PhantomLimit.cpp` — the dynamic (N-parameterized) cap-raise; offsets + trampolines
  - `OverhaulDLL/include/PhantomLimit.h`
  - `dev_scripts/MultiplePhantomsNotes.odt` — RE notes
- `C:\Users\treyf\Downloads\_overhaul_extract\Dark-Souls-1-Overhaul-0.01\OverhaulDLL\MultiPhantom.cpp`
  — the 0.01 snapshot (same offsets; full static + dynamic in one file)

> Note: Overhaul `master` targets Dark Souls **Remastered** (x64/d3d11) and is the WRONG target. Use the
> **Inferno** branch (x86 PTDE) only. The PTDE Steamworks exe has been frozen since 2016, so these offsets
> are valid for the live binary.

---

## 6. The offset facts (appendix)

Target: **x86 / Win32**, `ds1_base = GetModuleHandle(NULL)`. `N` = MaxPhantoms (stock 4).

Derived sizes:
- summon slot stride: `N * 0x20`
- `summon_chars_data` alloc: `16 + 1216 * N`  ; byte-length form: `1216 * N`
- `players_connected_array` alloc: `24 + 20*N + 36`
- inline-array offset shift: `pca_offset_add = 20 * (N - 4)`
- aggregate phantom check: `N * 7`
- max connection tickets: `N << 24`
- many bounds compare against `N`; some (phantom-only slots / `<=` checks) against `N - 1`

The **complete static patch list** (≈50 sites, each with RVA + bytes + verifyLen + note) is already
encoded in `mctde-PhantomUnleashed/PhantomLimit.cpp` — use that as the authoritative table; do not re-derive.
The **Stage 2** trampoline/detour offsets are listed in Requirement E and in the reference
`PhantomLimit.cpp` (the `inject_jmp_5b(... )` calls and the `pca_off*` functions).

---

## 7. Build & test plan (do in this order)

1. **Fold in (Req A)**, build mctde-Link Release|Win32. Confirm it still loads and the overlay works
   (no behavior change yet — `Mode=Off` or `VerifyOnly=1`).
2. **Verify pass:** set `Mode=On`, `VerifyOnly=1`, `MaxPhantoms=6`. Launch, read the log. Expect
   `VERIFY ok` for every static site, `0 mismatches`. **Any `VERIFY MISS` means an offset is wrong for
   this exe — stop and investigate before applying.**
3. **No-op sanity:** `VerifyOnly=0`, `MaxPhantoms=4`. Apply should change nothing meaningful; confirm the
   game runs and unload restores cleanly (check the restore log line). (Note: a few constants aren't exact
   `f(4)=vanilla`, so this is an approximate, not perfect, no-op.)
4. **Stage 2 (Req E):** add trampolines/detours + bounded AoB. Re-run verify, then apply with a small
   `MaxPhantoms` (e.g. 6) and test an actual >4 co-op session with another modded client.
5. **Prompt (Req B):** confirm the Yes/No box appears at launch; No → vanilla pool, nothing patched;
   Yes → patched + segregated.
6. **Segregation (Req C):** confirm a vanilla player cannot join a PhantomUnleashed session and is not
   disrupted; confirm two PhantomUnleashed players match.
7. **Teardown:** confirm patches revert on quit and on window-close (watchdog path); no zombie process.

---

## 8. Open questions to resolve before shipping

1. **Exact segregation mechanism** — do the connection/lobby patches alone segregate, or is an explicit
   pool key / matchmaking password needed to guarantee vanilla players are never pulled in? Confirm
   against the Inferno reference and in-game.
2. **Level-range write value** — `(N-1)<<6` (dynamic ref) vs `0x08000000` (static ref) for the
   BlackSos/Invade arrays. Pick per the dynamic reference and your N.
3. **Max N** — reference ships 18. Decide a safe shipping default and an enforced upper bound.
4. **Does any existing third-party `phantom_break.dll` still get chainloaded?** If PhantomUnleashed replaces
   it, make sure both aren't active at once (double-patch = crash). Consider detecting/refusing.

---

## 9. Constraints recap
- x86 / Win32 / v143, single `d3d9.dll` output.
- PTDE Steamworks exe (frozen since 2016); `ds1_base = GetModuleHandle(NULL)`.
- Clean-room from facts; credit Metal-Crow; never paste his source.
- Reversible, thread-safe, bounded, verify-before-apply, restore-on-teardown.
