# Releasing & Testing the Auto-Updater

The built-in version checker (`mctde-Link/VersionChecker.cpp`) both **detects** new
versions and **auto-installs** them. It runs on two channels chosen at compile time by
the `MCTDE_LINK_TEST_CHANNEL` macro, which the `.vcxproj` defines **only for Debug**:

| Build       | Reads manifest        | Downloads update from                         | Use            |
|-------------|-----------------------|-----------------------------------------------|----------------|
| **Release** | `latest.txt`          | `/releases/latest/download/mctde-Link.zip`    | Ship to users  |
| **Debug**   | `TestingVersion.txt`   | pre-release tagged `test` → `mctde-Link.zip`  | Sandbox testing |

Rule of thumb: **build Debug = sandbox, build Release = ship.** A pre-release never
becomes `/releases/latest`, and real users never read `TestingVersion.txt`, so testing
can never touch the installed population.

---

## How "latest" is detected

The DLL never computes the new version number to fetch a build. It downloads from a
permanent URL that GitHub always redirects to the newest release's asset:

```
https://github.com/McRoodyPoo/mctde-Link/releases/latest/download/mctde-Link.zip
```

So a user who skipped several versions still gets the current build from that one URL.
`latest.txt` only decides *whether* to prompt; the URL decides *what* to fetch.

The updater preserves the user's existing `mctde-link.ini` (never overwritten). Because
the game holds `d3d9.dll` locked, install happens in a generated detached PowerShell
helper that waits for the game to exit, swaps the files, relaunches, and self-deletes.

---

## Shipping a production release (checklist)

1. **Bump the installed-version constant** in `mctde-Link/VersionChecker.cpp`:
   - `CURRENT_MCTDE_LINK_VERSION` (and `CURRENT_MCTDE_VERSION` if mctde changed).
2. **Build Release** (`Release | x86`). Output: `Release/d3d9.dll`.
3. **Build the release zip** named exactly **`mctde-Link.zip`**, containing at the root
   (or inside one top-level folder):
   - `d3d9.dll` (the new Release build)
   - **`mctde_input.dll`**: a modern 32-bit Valve `steam_api.dll` renamed. Required by the
     `[Controller] BindingNudge` feature (PTDE's own `steam_api.dll`, an `ISteamController`
     v001 build, is too old to open the Steam controller binding panel). It must export the `SteamAPI_ISteamInput_*`
     flat functions and be **32-bit (PE32)**. A validated copy lives at
     `Z:\Dark Souls Mods\Dark-Souls-1-Overhaul-master\OverhaulDLL\lib\steam\lib\steam_api.dll`
     (FileVersion 06.28.18.86, `SteamInput002`/`SteamClient020`); any current Steam game's 32-bit
     `steam_api.dll` or the Steamworks SDK `redistributable_bin\steam_api.dll` also works.
     **If you swap in a different build, update `[Controller] BindingNudgeInputVersion` /
     `BindingNudgeClientVersion` to that SDK's interface versions** (the flat wrappers are version-coupled).
     Freely redistributable. If omitted, the controller nudge simply no-ops (logged); nothing else breaks.
   - optionally `mctde-link.ini` (won't overwrite an existing user ini) and any extras.
4. **Create a GitHub release** (a full release, *not* a pre-release) and **attach
   `mctde-Link.zip`** as an asset. The tag name doesn't matter to the updater.
5. **Update `latest.txt` on `main`** so `mctde-link=` matches the new version. This is
   what makes already-installed clients prompt.

Steps 3 and 4 are what `tools/package_release.sh` does for you (see Packaging below).

> ⚠️ If a release is missing the `mctde-Link.zip` asset, auto-update gets a 404 and
> falls back to opening the releases page (graceful, but no auto-install).

### Packaging

`tools/package_release.sh X.Y.Z` builds `dist/X.Y.Z/mctde-Link.zip` (+ `.sha256`) from
`Release/d3d9.dll` and `INPUT_DLL=<path to the 32-bit steam_api.dll>`, adding
`mctde-link.ini` (from `samples/mctde-link.sample.ini`), `README.md` and `CHANGELOG.md`. It refuses
to run unless `CURRENT_MCTDE_LINK_VERSION`, `version.rc`'s FILEVERSION and a `## vX.Y.Z` changelog
heading all say X.Y.Z, and both dlls are 32-bit, so a release can't go out missing a file again
(0.4.8 shipped without `mctde_input.dll`).

### Linux (Proton)

The same zip serves Linux: `d3d9.dll` runs inside Proton. The in-game updater can't swap the dll
there (its helper is PowerShell, which Proton lacks), so under Wine/Proton it leaves the update to
the launcher instead of closing the game. Both launchers (Windows under Proton, native Linux)
replace `d3d9.dll` from `mctde-Link.zip` when they open, with Auto-Update on.

---

## latest.txt: who reads what

`latest.txt` on `main` is the one manifest for the whole project. Versions are shared by Windows
and Linux; each updater picks the download for the OS it runs on.

| Key | Read by | Downloads |
|---|---|---|
| `mctde=` | in-game checker | opens the NexusMods page |
| `mctde-link=` | in-game checker, both launchers | `mctde-Link.zip` from this repo's latest release |
| `mctde-launcher=` | both launchers, installer (Linux launcher step) | `mctde-launcher-windows.zip` / `mctde-launcher-linux.zip` from mctde-Launcher's `v<ver>` release |
| `mctde-installer=` | installer | `mctde-Installer-windows.zip` / `mctde-Installer-linux.zip` from mctde-Installer's `v<ver>` release |
| `mctde-installer-linux=` | Linux installers older than 0.4.0 | keep equal to `mctde-installer=` |

Linux launchers older than 0.6.0 read a separate manifest from mctde-Launcher's `linux` branch;
mctde-Launcher's `tools/package_release.sh` regenerates it. Release order for any component:
publish the GitHub release (with every asset its script lists) **first**, then bump its line here.
Each repo's `tools/package_release.sh` prints its own exact steps.

---

## Testing the auto-updater (sandbox, zero user impact)

1. **Build Debug** (`Debug | x86`) → `Debug/d3d9.dll`. Drop it in your test `DATA` folder.
   (Debug needs the debug VC runtime; fine on a dev machine, don't hand it to users.)
2. **Create a GitHub pre-release tagged exactly `test`** and attach a `mctde-Link.zip`
   containing the `d3d9.dll` you want it to install. Mark it **pre-release**.
3. **Edit `TestingVersion.txt` on `main`** so `mctde-link=` is higher than the Debug
   DLL's `CURRENT_MCTDE_LINK_VERSION`.
4. **Launch the game.** A `[TEST]` update prompt fires:
   - **Yes** → downloads the `test` zip, closes the game, swaps in the new DLL, relaunches.
   - **No** → opens the releases page and closes the game.
5. **Iterate** by re-uploading the asset on the same `test` pre-release and editing
   `TestingVersion.txt`. Enable `[Settings] EnableLogging=1` to get step-by-step traces
   in `VersionCheck.log` (the log header line shows `Channel: TEST`).
