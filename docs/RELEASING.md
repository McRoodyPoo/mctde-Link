# Releasing & Testing the Auto-Updater

The built-in version checker (`mctde-Link/VersionChecker.cpp`) both **detects** new
versions and **auto-installs** them. It runs on two channels chosen at compile time by
the `MCTDE_LINK_TEST_CHANNEL` macro, which the `.vcxproj` defines **only for Debug**:

| Build       | Reads manifest        | Downloads update from                         | Use            |
|-------------|-----------------------|-----------------------------------------------|----------------|
| **Release** | `latest.txt`          | `/releases/latest/download/mctde-Link.zip`    | Ship to users  |
| **Debug**   | `LatestVersion.txt`   | pre-release tagged `test` → `mctde-Link.zip`  | Sandbox testing |

Rule of thumb: **build Debug = sandbox, build Release = ship.** A pre-release never
becomes `/releases/latest`, and real users never read `LatestVersion.txt`, so testing
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
   - optionally `mctde-link.ini` (won't overwrite an existing user ini) and any extras.
4. **Create a GitHub release** (a full release, *not* a pre-release) and **attach
   `mctde-Link.zip`** as an asset. The tag name doesn't matter to the updater.
5. **Update `latest.txt` on `main`** so `mctde-link=` matches the new version. This is
   what makes already-installed clients prompt.

> ⚠️ If a release is missing the `mctde-Link.zip` asset, auto-update gets a 404 and
> falls back to opening the releases page (graceful, but no auto-install).

---

## Testing the auto-updater (sandbox, zero user impact)

1. **Build Debug** (`Debug | x86`) → `Debug/d3d9.dll`. Drop it in your test `DATA` folder.
   (Debug needs the debug VC runtime — fine on a dev machine, don't hand it to users.)
2. **Create a GitHub pre-release tagged exactly `test`** and attach a `mctde-Link.zip`
   containing the `d3d9.dll` you want it to install. Mark it **pre-release**.
3. **Edit `LatestVersion.txt` on `main`** so `mctde-link=` is higher than the Debug
   DLL's `CURRENT_MCTDE_LINK_VERSION`.
4. **Launch the game.** A `[TEST]` update prompt fires:
   - **Yes** → downloads the `test` zip, closes the game, swaps in the new DLL, relaunches.
   - **No** → opens the releases page and closes the game.
5. **Iterate** by re-uploading the asset on the same `test` pre-release and editing
   `LatestVersion.txt`. Enable `[Settings] EnableLogging=1` to get step-by-step traces
   in `VersionCheck.log` (the log header line shows `Channel: TEST`).
