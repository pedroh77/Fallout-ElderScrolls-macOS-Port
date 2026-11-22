# Fallout 3 & Fallout: New Vegas for macOS (Apple Silicon)

Native Apple Silicon (`arm64`) macOS ports of **Fallout 3: Game of the Year Edition** and **Fallout: New Vegas**, built with **x86-to-C/ARM64 static recompilation** and direct **Direct3D 9 → Apple Metal** translation. Each game is packaged as its own standalone bundle (`Fallout 3.app`, `Fallout New Vegas.app`) with no Wine, CrossOver, Rosetta 2, or virtual machines required.

Both games are translated by the same recompiler and installed by the same `quick-install.sh`; the script identifies the game from the executable or GOG installer it finds and asks which one to build when several installations are present.

## Quick Start

**What you need upfront:**
1. An **Apple Silicon Mac** (M1 or newer) running **macOS 14 (Sonoma)** or later.
2. **Apple Command Line Tools** (`xcode-select --install`) and **[Homebrew](https://brew.sh/)** — `quick-install.sh` automatically installs every other build tool it needs (`ghidra`, `unicorn`, `cmake`, `ffmpeg`, plus `innoextract` for GOG installers or `mono` for Steam DRM) and none of them are required to play once the `.app` is built (see [Notes on Cleanup](#notes-on-cleanup)).
3. **Your own PC copy** of Fallout 3 GOTY or Fallout: New Vegas:
   - **GOG:** download the offline backup installer (`setup_fallout_*.exe` + `.bin` files) into `~/Downloads/`. No manual extraction needed.
   - **Steam:** download the Windows game files via `steamcmd` (see [Obtaining the Windows Game Files on macOS](#obtaining-the-windows-game-files-on-macos)).

**Build & install:**

```sh
./quick-install.sh
```

The finished app is placed in `~/Applications/Fallout 3.app` or `~/Applications/Fallout New Vegas.app`.

---

## Why This Port Exists (Wine Challenges vs. Recent Improvements)

*Fallout 3* and *Fallout: New Vegas* share the Gamebryo engine and the same three traits that have historically made them a worst-case scenario under Wine on Apple Silicon:

1. **32-bit x86 executable**
2. **Heavy x87 (80-bit) floating-point usage** — which Rosetta 2 emulates slowly in software, requiring external hooks like `rosettax87_jit` or `x87sidecar` (rarely bundled in standard Wine wrappers).
3. **Direct3D 9** — which is unsupported by **D3DMetal (GPTK)** and **DXMT** (both focused on Direct3D 10/11/12), while the older macOS DXVK fork had limitations on top of MoltenVK.

While the Wine ecosystem has improved recently — with **Wine ARM64 + FEX** in reduced-precision mode (such as CodeWeavers' CrossOver Preview) and new D3D9-to-Metal renderers (such as `mtld3d` / `Mach9`) — many free wrappers (like Sikarugir) still rely on traditional Intel Wine + Rosetta 2.

These ports eliminate that entire stack at the root:
* **No Rosetta 2 or FEX overhead:** game code is statically recompiled to **native ARM64**, with x87 arithmetic lowered to 64-bit `double` (53-bit precision, matching what the MSVC CRT configures on Windows).
* **Lower RAM footprint:** running as a single native process without `wineserver` or x86 translation layers saves significant memory — especially valuable on **8 GB unified-memory** Apple Silicon Macs.
* **Direct D3D9 → Metal:** D3D9 calls and SM 1–3 shaders compile directly to Metal (`d3d9_metal.m` and `sm2msl.c`).

---

## What Changed Compared to Upstream

This repository adapts **[FNV-iOS-Shim](https://github.com/arjuk95-dot/FNV-iOS-Shim)** (which itself builds on **[Fallout-NV-MacOS](https://github.com/thetheoryofR/Fallout-NV-MacOS)**) back into desktop **macOS** ports, and extends the recompiler from *Fallout: New Vegas* to *Fallout 3*:

* **Build target switched from iOS (UIKit) to macOS (AppKit):**
  * Removed the `ios/` layer (touch controls, overlay editor, gyro handling, `verify_ipa.py`, and touch UI tests).
  * Added the `macos/` layer with an AppKit window (`FNVApp.m`), **macOS Game Mode** support (`LSApplicationCategoryType: public.app-category.games`), physical panel resolution detection (avoids rendering at inflated scaled HiDPI resolutions, with `FNV_OUTPUT_WIDTH`/`FNV_OUTPUT_HEIGHT` overrides), automatic relative mouse capture while focused (released on `Cmd+Tab`), DirectInput keyboard mapping (`dinput.c` / `input.c`), and `GameController.framework` support (`FNVPad.c`).
  * Retained `FNV-iOS-Shim` media and stability improvements: native PCM mixer (`FNVAudioMixer.c` + `AVAudioEngine`), Ogg/Vorbis decoding, DirectShow pull-source (`dshow.m`), native `AVFoundation` cutscene playback (`FNVNativeVideo.m`), and heap/texture/sync fixes.
* **Restored Unicorn Engine fallback:**
  * `FNV-iOS-Shim` had removed Unicorn fallback execution due to iOS restrictions. This port restores `upstream/recomp/host/fallback.c` and links `libunicorn.a` to handle any uncompiled code paths or edge-case instructions.
* **Precomputed symbol maps for four executables (`symbols/fallout3/`, `symbols/fallout3-steam/`, `symbols/fnv/`, `symbols/fnv-steam/`):**
  * The original iOS project shipped without symbol maps and required a full Ghidra auto-analysis (~40 minutes) even though it only targeted a single executable.
  * Precomputed function and jump-table metadata for the **GOG and Steam** builds of both games plus `tools/ghidra/ApplyMetadataAndDump.java` cut the Ghidra step down to **~1 min** P-code export + **~3 min** C emission. `tools/ghidra/ExportSymbols.java` exports a new map from an analysed executable, so another build can be added without redoing the analysis on every install. For DRM-free executables outside the precomputed set, the full Ghidra pipeline (`FixNoReturn.java`, `ResolveJumpTables.java`, `FixEntries.java`, `DumpPcode.java`) remains available automatically as a fallback.
* **One-command installer & packager (`quick-install.sh`):**
  * Auto-detects **both games** across common locations (GOG offline `setup_fallout_*.exe` installers, Steam, SteamCMD, DepotDownloader, previously extracted GOG folders, `~/Downloads`, `~/Games`, or an existing `.app`), scans every candidate instead of stopping at the first hit, and presents a numbered menu when more than one installation is present — for example *Fallout 3: Game of the Year Edition (GOG installer)* and *Fallout: New Vegas (Steam)*. `--game fnv|fallout3` restricts the search to one game.
  * Extracts GOG offline installers directly via `innoextract --exclude-temp` into a temporary directory (installed via Homebrew on demand, skipping ~350 MB of Windows redistributables), checks for SteamStub `.bind` DRM (offering to back up the executable to `.exe.og` and strip the DRM using official Steamless CLI + Mono on demand), verifies the executable SHA-256, installs missing Homebrew dependencies (`ghidra`, `unicorn`, `cmake`, `ffmpeg`, plus `innoextract`/`mono` only when needed), caches codecs/objects/translations under `~/Library/Caches/fnv-macos/`, moves game data into `<App>.app/Contents/Resources/GameData/`, converts `.bik` cutscenes to `.mp4` via `ffmpeg`, cleans up leftover Windows source files and Steam `appmanifest_<appid>.acf`, ad-hoc signs the bundle, and optionally wraps it in a `.dmg` (`--dmg`).
* **Additional codebase changes:**
  * **Optional Steamworks bridge & Overlay launcher (Steam builds only):** `upstream/recomp/host/shims_steam.c` loads Valve's official ARM64 `libsteam_api.dylib` (bundled only for Steam builds, omitted for GOG or `--no-steam`) to bridge `steam_api.dll` (`__thiscall` proxies for `ISteamUser`, `ISteamUtils`, `ISteamUserStats`, `ISteamApps`, and `ISteamFriends`, plus `SteamAPI_ManualDispatch_*` callbacks) for Steam achievements and status, with clean offline fallback. `macos/FNVSteamLauncher.c` starts the Mac Steam client silently (`-silent`), waits for global user login (`ConnectToGlobalUser`) before launching the game, and injects `gameoverlayrenderer.dylib` for the Metal Steam Overlay (`Shift+Tab`) and, where the game exposes Steamworks (Fallout: New Vegas), achievement notifications. The launcher takes the target executable and AppID from the build (`FalloutNewVegas`/22380 for FNV, `Fallout3`/22370 for Fallout 3).
  * **Fallout 3 specifics:** per-game title, save/log directories, and a `plugins.txt` bootstrap seeded from `Data/DLCList.txt` so all five GOTY add-ons load; xlive (Games for Windows - LIVE) stubs for the GOG build and winsock offline stubs for both; per-game `.icns` icons (`macos/Assets/AppIcon.png` for FNV, `macos/Assets/AppIcon-Fallout3.png` for Fallout 3).
  * **User data outside the bundle (`FNV_USER_DIR`):** updates in `upstream/recomp/host/shims_files.c` and `shims_win.c` (`SHGetFolderPathA`) route `UserDocuments` (saves, `FALLOUT.INI`, `FalloutPrefs.ini`) to `~/Library/Application Support/FalloutNV/UserDocuments` or `~/Library/Application Support/Fallout3/UserDocuments`, with logs in `~/Library/Logs/FalloutNV/fnv.log` or `~/Library/Logs/Fallout3/fallout3.log`.
  * **macOS 14+ Metal compatibility (`d3d9_metal.m`):** guarded `copts.mathMode = MTLMathModeSafe` with compile-time SDK and runtime `@available(macOS 15.0, iOS 18.0, *)` checks (falling back to `copts.fastMathEnabled = NO`) so the port builds and runs cleanly on macOS 14 (Sonoma) and newer.
  * **Build & test suite updates (`tools/build.py`, `tools/app_icon.py`, `tests/`):** updated build scripts and native/host test suite for the macOS target; `tools/build.py --game fallout3` selects the Fallout 3 profile (bundle identifier `dev.f3mac.fallout3`, executable name, define, icon).

---

## Requirements & Supported Builds

* **Apple Silicon Mac (`arm64`)** running **macOS 14 (Sonoma)** or later.
* **Xcode Command Line Tools** (`xcode-select --install`) and **Homebrew**.
* **Your own PC copy** of at least one of the two games:

### Fallout 3: Game of the Year Edition

| Store | Executable | Version | SHA-256 | Symbols |
|---|---|---|---|---|
| **GOG** | `Fallout3.exe` | 1.7.0.3 | `df9f0e1ec2c5413a9cef2db2dde6d926b99707d528049af4eb15538587d55f99` | `symbols/fallout3/` |
| **Steam** | `Fallout3.exe` | 1.7.0.4 | `c3f97c2255fa041a851c17cf372d69aaadd8694e2dc4230ba556001bbfbd2f3e` | `symbols/fallout3-steam/` |

* The precomputed symbols cover the **GOG 1.7.0.3** installer (`setup_fallout_3_1.7.0.3_(12034).exe`) and the current **Steam 1.7.0.4** build (the 2021 patch that removed Games for Windows - LIVE). The GOTY edition bundles all five add-ons (Operation: Anchorage, The Pitt, Broken Steel, Point Lookout, Mothership Zeta).
* Steam's **22370** is the Game of the Year Edition. **22300** is the original 2008 release, which is still listed on Steam; its executable may not match the precomputed SHA-256, in which case `quick-install.sh` falls back to full Ghidra auto-analysis.
* Our Steam GOTY copy ships without a SteamStub `.bind` section. If yours is still packed, the installer detects the DRM and can strip it with Steamless exactly as for FNV.
* **Standalone offline by default (no achievements or Steam Cloud):** the 2021 Steam patch removed GFWL without adding Steamworks achievements or Steam Cloud (the Steam page lists only Single-player/Family Sharing), and the GOG release lists no achievements either. Because of that, `quick-install.sh` builds Fallout 3 as a standalone offline app by default; pass `--with-steam` if you want the Steam Overlay (`Shift+Tab`) and playtime/status tracking.

### Fallout: New Vegas

| Store | Executable | SHA-256 | Symbols |
|---|---|---|---|
| **GOG** | `FalloutNV.exe` | `89e7020487e80f20ddc92ea641c4433b2c6839a82f45a076e414d167d11dd0fe` | `symbols/fnv/` |
| **Steam** (after Steamless) | `FalloutNV.exe` | `19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f` | `symbols/fnv-steam/` |
| **Steam** (original, SteamStub v2.1) | `FalloutNV.exe` | `3a87f92f011e5dc9179ddf733cf08be2b39ea6e5b7a8a9e3a9a72dafcc1b104d` | strip first |

* GOG builds launch `FalloutNewVegas` directly and completely standalone — no Steam launch, no Steam Overlay, and no `libsteam_api.dylib`. GOG Galaxy achievements/cloud saves are not implemented in this port, so achievements are available in the Steam build only.
* For Steam builds, when `quick-install.sh` detects SteamStub DRM it warns you and asks for confirmation (or pass `--unpack-drm`) to automatically install `mono` (via Homebrew if missing), download the official [Steamless](https://github.com/atom0s/Steamless) CLI release (`v3.1.0.5`, verified by SHA-256), back up the original file to `FalloutNV.exe.og`, strip the DRM, and compile using `symbols/fnv-steam/`.
* **Steam Achievements & Overlay:** `quick-install.sh` fetches Valve's official ARM64 `libsteam_api.dylib` (from Steamworks.NET 20.1.0, verified by SHA-256) into `Contents/Frameworks/` and bundles `FalloutNewVegasLauncher` so double-clicking `Fallout New Vegas.app` automatically starts the Mac Steam client (if installed) and enables the game's **75 Steam achievements** (base game + DLC), Steam status, and the Steam Overlay (`Shift+Tab`). Pass `--no-steam` at build time to omit Steam integration, or set `FNV_STEAM_NO_OVERLAY=1` / `FNV_STEAM_DISABLE=1` at runtime.
* **Epic Games Store / other DRM-free builds:** if the SHA-256 differs from the GOG and Steam builds above, `quick-install.sh` automatically falls back to full Ghidra auto-analysis (slower, and unsupported builds may require address adjustments if the binary layout differs).

---

## Obtaining the Windows Game Files on macOS

Because both games are Windows-only on Steam and GOG, you can fetch or extract the game files directly on your Mac using any of the methods below. All output paths shown here are auto-detected by `quick-install.sh`.

### Option 1: Steam

> **AppID note:** Fallout 3 is **22370** (Game of the Year Edition, includes all DLC) or **22300** (original 2008 release). Fallout: New Vegas is **22380** (standard global edition); use **22490** instead if your Steam account owns the regional PCR edition.

#### Method A — `steamcmd` (recommended)

```sh
brew install --cask steamcmd
# Fallout 3: Game of the Year Edition
steamcmd +@sSteamCmdForcePlatformType windows +login <YOUR_STEAM_USERNAME> +app_update 22370 validate +quit
# Fallout: New Vegas
steamcmd +@sSteamCmdForcePlatformType windows +login <YOUR_STEAM_USERNAME> +app_update 22380 validate +quit
```

*This downloads the game (along with any DLCs owned by your account) directly into `~/Library/Application Support/Steam/steamapps/common/Fallout 3 goty` (22300 installs as `.../common/Fallout 3`) / `.../Fallout New Vegas` (or the equivalent under `~/Steam/steamapps/common/`), both of which `quick-install.sh` auto-detects. Pass `+force_install_dir "$HOME/Downloads/Fallout 3"` (or `.../Fallout New Vegas`) before `+login` if you prefer a separate folder.*

#### Method B — `DepotDownloader`

```sh
brew tap steamre/tools
brew install depotdownloader
# Fallout 3: Game of the Year Edition
depotdownloader -app 22370 -os windows -username <YOUR_STEAM_USERNAME> \
  -dir "$HOME/Downloads/Fallout 3"
# Fallout: New Vegas
depotdownloader -app 22380 -os windows -username <YOUR_STEAM_USERNAME> \
  -dir "$HOME/Downloads/Fallout New Vegas"
```

*If you omit `-dir`, DepotDownloader places files under `./depots/<appid>/<manifest>/` (`~/depots/22370/`, `~/Downloads/depots/22370/`, the same paths for `22300`, `22380`, `22490` are all auto-detected).*

#### Method C — Steam Client Console

1. Open the hidden Console tab in the desktop Steam client:
   ```sh
   open "steam://open/console"
   ```
2. Download the game depots (plus any DLC depots you own — check the app's depot IDs on SteamDB if needed):
   ```text
   download_depot 22370 <DEPOT_ID>
   download_depot 22380 <DEPOT_ID>
   ```
3. Steam saves each depot separately under:
   `~/Library/Application Support/Steam/steamapps/content/app_<APPID>/depot_*/`
   Merge the contents of those `depot_*` folders into one folder (for example `~/Downloads/Fallout 3/`) so `Fallout3.exe`/`FalloutNV.exe` and `Data/` sit together.

### Option 2: GOG Offline Installer (Easiest — No Manual Extraction)

Download the GOG offline backup installer files from your GOG library into `~/Downloads/`:
* **Fallout 3: Game of the Year Edition:** `setup_fallout_3_1.7.0.3_(12034).exe` plus its `.bin` parts (`-1.bin`, `-2.bin`) in the same folder.
* **Fallout: New Vegas:** `setup_fallout_new_vegas_*.exe` plus its `.bin` parts in the same folder.

Then just run `./quick-install.sh` (or pass the `.exe` directly, e.g. `./quick-install.sh ~/Downloads/setup_fallout_3_*.exe`). The script installs `innoextract` via Homebrew if needed, extracts only the playable game files into a temporary directory (skipping Windows redistributables), and cleans up the temporary folder automatically. Folders you already extracted manually with `innoextract` are still auto-detected as well.

---

## Quick Install

```sh
# Auto-detect either game (installed folders or GOG setup_*.exe in ~/Downloads).
# If more than one copy is found, a numbered menu asks which one to build.
./quick-install.sh

# Restrict detection to one game
./quick-install.sh --game fallout3
./quick-install.sh --game fnv

# Or pass an explicit path (GOG setup_*.exe, game folder, or parent folder)
./quick-install.sh ~/Downloads/setup_fallout_3_*.exe
./quick-install.sh "/path/to/Fallout New Vegas"
```

When more than one installation is found, the selector looks like this:

```text
Multiple game installations found:
  1) Fallout: New Vegas (Steam) — /Users/you/Library/Application Support/Steam/steamapps/common/Fallout New Vegas
  2) Fallout 3: Game of the Year Edition (GOG installer) — /Users/you/Downloads/setup_fallout_3_1.7.0.3_(12034).exe
  Select the game to build [1-2]:
```

In a non-interactive shell the script stops with the same list and asks you to pass a path or `--game`.

Steam integration flags:

```sh
# Fallout: New Vegas — opt out of Steam launcher/Overlay/achievements (offline standalone)
./quick-install.sh --no-steam

# Fallout 3 — opt into Steam launcher/Overlay/playtime (standalone offline by default)
./quick-install.sh --with-steam
```

To skip the confirmation prompt when stripping SteamStub DRM, wrap the app in a drag-to-Applications `.dmg`, or keep the build cache for faster incremental rebuilds during development:

```sh
./quick-install.sh --unpack-drm --dmg
./quick-install.sh --keep-cache
```

> **Note on disk usage & automatic cleanup:** to avoid duplicating roughly 9–10 GB per game, `quick-install.sh` **moves** the playable game files into `~/Applications/Fallout 3.app` or `~/Applications/Fallout New Vegas.app` (`Contents/Resources/GameData/`), **deletes leftover Windows-only files** from the source folder (extracted GOG folders, Steam `steamapps/common/` folders, or DepotDownloader folders), removes Steam's matching `appmanifest_<appid>.acf` so Steam does not keep an uninstallable ghost entry in your Mac library (Steam achievements, Overlay, playtime, and "Playing" status do not depend on the `.acf` file), and **deletes the build/translation cache (`~/Library/Caches/fnv-macos`) automatically** once the app is built and signed (pass `--keep-cache` or set `FNV_KEEP_CACHE=1` to keep it for faster rebuilds). Original GOG offline installers (`setup_*.exe` + `.bin`) and existing `.app` bundles are **not** deleted automatically.

---

## Notes on Cleanup

**None of the build tools are needed to play the game once the `.app` is built.** `libunicorn.a` is statically linked into the app binary, `.bik` cutscenes are already converted to `.mp4`, and Ghidra only runs during translation.

`quick-install.sh` cleans up leftover source files and `~/Library/Caches/fnv-macos` automatically upon success (unless `--keep-cache` is passed), and intentionally does **not** uninstall global software from your Mac. At the end of a run, it prints the exact list of any Homebrew packages it installed together with a ready-to-copy `brew uninstall` command.

If you installed build tools only for this port and want to remove them later:

1. **Homebrew packages used for building/downloading** (or run the exact `brew uninstall ...` command printed at the end of `quick-install.sh`; edit this list to omit anything you want to keep):
   ```sh
   for p in ghidra unicorn cmake ffmpeg mono innoextract depotdownloader steamcmd; do brew list "$p" &>/dev/null && brew uninstall "$p"; done && brew cleanup
   ```
   Optional: if you added the DepotDownloader tap:
   ```sh
   brew untap steamre/tools 2>/dev/null
   ```

2. **Build & translation cache** (already removed automatically unless you passed `--keep-cache` or an earlier run stopped mid-way):
   ```sh
   rm -rf ~/Library/Caches/fnv-macos
   ```

3. **Original GOG offline installers** in `~/Downloads/` (`setup_fallout_*.exe` and `.bin` parts), if you do not need to keep a backup.

4. **Homebrew & Apple Command Line Tools (optional — only if you installed them solely for this port):**
   * Uninstall Homebrew using the [official Homebrew uninstall script](https://github.com/homebrew/install#uninstall-homebrew).
   * Uninstall Apple Command Line Tools:
     ```sh
     sudo rm -rf /Library/Developer/CommandLineTools
     ```

**Keep these files to continue playing:**
* `~/Applications/Fallout 3.app` and/or `~/Applications/Fallout New Vegas.app` (the game itself).
* `~/Library/Application Support/Fallout3/` and/or `~/Library/Application Support/FalloutNV/` (saves and `.ini` settings).

---

## Manual Build & Tests

```sh
# Compile host-only check (no game files required)
python3 tools/build.py host-check

# Build standalone Metal/input probe app
python3 tools/build.py probe --out build/probe-test

# Run native host/media test suite
bash tests/run.sh
```

---

## Credits

* **[Fallout-NV-MacOS](https://github.com/thetheoryofR/Fallout-NV-MacOS)** (by **thetheoryofR**) — Original x86→C/ARM64 static recompiler, D3D9→Metal renderer, Win32/COM host, and Unicorn fallback (MIT license in `upstream/LICENSE`).
* **[FNV-iOS-Shim](https://github.com/arjuk95-dot/FNV-iOS-Shim)** (by **arjuk95-dot**) — Native audio mixer (`AVAudioEngine`), Ogg/Vorbis bridge, `AVFoundation` video, DirectShow pull-source, and heap/texture/sync fixes.
* **[macIV](https://github.com/tanmayisdumb/macIV)** — App bundle packaging and `quick-install.sh` UX inspiration.
