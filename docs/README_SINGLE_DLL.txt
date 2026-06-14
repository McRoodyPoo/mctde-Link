mctde-Link single-DLL project
=============================

This project builds one runtime DLL: d3d9.dll.

Built into that one DLL:
- d3d9 proxy/forwarder
- MCTDE_NetOverlay
- VersionChecker
- compatibility chainloader for DllMain-driven third-party DLLs

Build:
- Open mctde-Link.sln
- Select Release | x86
- Build

Deploy beside DATA.exe:
- d3d9.dll
- MCTDE_D3D9_Hub.ini is still useful for overlay settings and optional compatibility DLL loading.

Do not deploy separate MCTDE_NetOverlay.dll or VersionChecker.dll for this single-DLL build.

Compatibility loading
---------------------

mctde-Link now supports third-party DLLs even in the single-DLL build.
This is mainly for tools such as PTDE Practice Tool / TAS Runner, which can initialize from DllMain and may not export McTDE's initialize_plugin function.

Supported options:

1. Normal side-by-side install:
   - Put mctde-Link's d3d9.dll beside DATA.exe.
   - Put PTDE Practice Tool's dinput8.dll beside DATA.exe.

2. mctde-Link chainload folder:
   - Put third-party DLLs in DATA.exe\mctde-Link_Chainload\.
   - mctde-Link loads every DLL in that folder in deterministic filename order.
   - d3d9.dll is intentionally skipped to avoid recursive proxy loading.

3. INI explicit loading:

   [DLLs]
   GenericDLL0=.\dinput8.dll
   GenericDLL1=.\SomeOtherTool.dll

   [Compatibility]
   ChainloadFolder=mctde-Link_Chainload

Notes:
- DLLs without initialize_plugin are no longer treated as failures. mctde-Link logs that they were left loaded as DllMain-only compatibility DLLs.
- The compatibility chainload pass runs before McTDE's delayed overlay hook setup and is also guarded from Direct3DCreate9/Direct3DCreate9Ex, so DX9 hook DLLs have a chance to install before the game creates the device.
- If PTDE Practice Tool is installed as root dinput8.dll, you usually do not need to list it in the mctde-Link INI. The explicit/folder options are for packed installs or launch setups where mctde-Link is responsible for loading the tool.
