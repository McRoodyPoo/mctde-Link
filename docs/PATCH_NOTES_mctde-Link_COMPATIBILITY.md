# mctde-Link compatibility patch

Goal: make mctde-Link coexist cleanly with PTDE Practice Tool / TAS Runner.

Why this was needed:
- mctde-Link is a single `d3d9.dll` proxy with the McTDE overlay and version checker built in.
- PTDE Practice Tool / TAS Runner is a `dinput8.dll` hudhook / DirectX9 ImGui overlay.
- The existing single-DLL mctde-Link path started only its built-in modules and did not keep the old generic DLL loading path useful for DllMain-driven third-party DLLs.
- Third-party DLLs such as PTDE Practice Tool may initialize entirely from DllMain and may not export McTDE's `initialize_plugin` function.

Changed file:
- `mctde-Link/dllmain.cpp`

What changed:
1. Added a compatibility chainload pass guarded by `LoadCompatibilityDllsOnce()`.
2. The chainload pass runs before McTDE's built-in delayed overlay hook setup.
3. Direct3DCreate9 and Direct3DCreate9Ex also trigger the guarded chainload pass before forwarding to the real system d3d9.dll.
4. `[DLLs] GenericDLL0..31` are supported again in the single-DLL build.
5. Added `[Compatibility] ChainloadFolder=mctde-Link_Chainload` support.
6. DLLs without `initialize_plugin` are no longer treated as failed plugin loads. They are logged as DllMain-only compatibility DLLs and left loaded.
7. Relative DLL paths are resolved beside DATA.exe instead of relying only on the current working directory.
8. `d3d9.dll` is intentionally skipped in the compatibility chainloader to avoid recursive d3d9 proxy loading.
9. Re-entrant chainload calls return immediately instead of waiting, avoiding LoadLibrary/DllMain deadlock hazards.

Recommended install:
- Put mctde-Link `d3d9.dll` beside `DATA.exe`.
- Put PTDE Practice Tool `dinput8.dll` beside `DATA.exe`.

Alternative packed install:
- Put PTDE Practice Tool's DLL in `DATA.exe\mctde-Link_Chainload\`.
- Or add it explicitly to `MCTDE_D3D9_Hub.ini`:

```ini
[DLLs]
GenericDLL0=.\dinput8.dll

[Compatibility]
ChainloadFolder=mctde-Link_Chainload
```

Build note:
- I could not run MSVC in this Linux sandbox, so this is a source patch, not a locally compiled `d3d9.dll` binary.
- Build in Visual Studio as `Release | Win32` / x86.
