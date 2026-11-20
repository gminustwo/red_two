# Red Two

[![release](https://img.shields.io/github/v/release/gminustwo/red_two?filter=v%2A&sort=semver&display_name=tag&label=release&color=orange)](https://github.com/gminustwo/red_two/releases/latest)
[![downloads](https://img.shields.io/github/downloads/gminustwo/red_two/total?label=downloads&color=brightgreen)](https://github.com/gminustwo/red_two/releases)

A static recompilation of **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube) for PC.

The game's own PowerPC code is translated to C++ ahead of time and runs on a native runtime
that stands in for the console's hardware. **No game data is included**: you need your own disc
image of the USA release (`GSWE64`, revision 0), for the downloads as for your own build.

**Downloads**: Windows (zip) and Linux / Steam Deck (AppImage) builds are on the
[Releases](../../releases) page. They contain the game's code in recompiled form, none of its data:
start it and choose your disc image (`.iso`, `.gcm`, `.rvz` or `.wia`, wherever it is) on the
launcher's disc page: type or paste its path, pick it in the file dialog, or find it in the
built-in browser (controller-friendly). Red Two remembers it; one next to the program is also
found by itself.
Or build Red Two yourself (below): the build recompiles the game from your disc. The Windows zip
is portable (everything of yours stays in its `user` folder) and brings the Visual C++ runtime
with it; it needs a CPU with AVX2 (Intel Haswell / AMD Ryzen or newer) and says so otherwise.

> Status: playable on Windows and Linux (also packaged as an AppImage for the Steam Deck):
> menus, cutscenes and missions at full speed with graphics (Aurora), sound (an emulation of the
> MusyX audio DSP), controls, saving, save states, widescreen, mods, cheats and RetroAchievements.

## Building (Windows)

Requirements: Visual Studio 2022 Build Tools (C++), CMake 3.25+, Ninja, clang-cl (LLVM).

```powershell
# 1. Build: recompiles the game straight from your disc image (RVZ, WIA, ISO or GCM)
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Disc "<your Rogue Leader.rvz>"

# 2. Run
build\windows\red_two.exe --disc "<your Rogue Leader.rvz>"
```

Without `--disc` the game takes the disc chosen in the launcher before (F1 > DISPLAY > Game disc),
else looks for `orig\GSWE64.iso`, `orig\GSWE64.rvz`, `GSWE64.rvz`, `GSWE64.iso` and any GSWE64
image next to the program or in the user folder, else the launcher asks for it.
`tools\rvz.py` (Python + libzstd) can still extract a disc or convert it to ISO.

The code generated from your disc ends up in `build/` and is yours alone: never share it.

`tools\make_release_win.ps1` packs a build into `build\RedTwo-windows-x64.zip` (the program, its
symbols, DLLs, a readme) for your own use on another PC.

## Building (Linux) and the Steam Deck AppImage

Requirements: clang, lld, CMake 3.25+, Ninja, git, and SDL3's build dependencies (X11, Wayland,
xkbcommon, ALSA / PulseAudio / PipeWire, udev and D-Bus development packages). Ubuntu 24.04 (also
under WSL2) works; the result needs glibc 2.38 or later (SteamOS 3.6+).

```bash
tools/build_linux.sh --disc "<your Rogue Leader.rvz>"     # -> build/linux/red_two
# an AppImage: the AppImage type 2 runtime (github.com/AppImage/type2-runtime, runtime-x86_64),
# mksquashfs and patchelf
tools/make_appimage.sh --runtime runtime-x86_64 --icon <256x256 png>   # -> build/linux/RedTwo-x86_64.AppImage
```

On the Steam Deck: copy the AppImage and your disc image (`.rvz` / `.iso`, any name) into the
same folder (or the disc into `~/Emulation/roms/gc`), make the AppImage executable, and add it
to Steam as a non-Steam game. The first start on a Deck is fullscreen with the picture as wide
as the screen. Settings, saves, states and logs live in `~/.local/share/Red Two/`.

## Building (macOS, Apple Silicon)

Requirements: the Xcode command line tools (`xcode-select --install`), CMake 3.25+ and Ninja
(`brew install cmake ninja`). Dawn (Metal), SDL3 and the other libraries are fetched and linked
in statically: the app needs nothing installed to run.

```bash
tools/build_macos.sh --disc "<your Rogue Leader.rvz>"     # -> build/macos/Red Two.app
```

Copy `Red Two.app` anywhere; a disc image in the same folder as the app is found by itself.
Settings, saves, states and logs live in `~/Library/Application Support/Red Two/`. The arm64
loader allows only position-independent programs, so (as on Android) a save state loads only
into the session that made it; the game's own saves on the memory card are not affected. The
app is signed ad hoc for this Mac: on another one, right-click > Open the first time.

### Playing

- **Launcher**: Red Two opens on its settings (the same pages as the in-game menu) before the
  game starts. Start game (the first row) or the controller's START launches it. Turn off
  "Show launcher at start" there to go straight into the game from then on; the in-game menu
  (F1 or Back) has the same switch and "Back to launcher". `--launcher` / `--no-launcher` override
  the setting for one start. A reset from the menu goes straight back into the game.
- Gamepads work through SDL (with rumble), also while the window is in the background. On an
  Xbox-style pad the face buttons follow the GameCube positions: A = A (fire), X (left) = B,
  B (right) = X, Y = Y; LT/RT = L/R, RB = Z, right stick = C-stick, Menu = Start. Keyboard: WASD or arrows steer, Space = A (fire),
  Left Shift = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick, TFGH = D-pad.
- The game's button prompts show your controller's buttons (the one each GameCube button is
  bound to, e.g. "PRESS (Y)" and SELECT CRAFT's "(B) > pilot"): the PlayStation symbols on a
  PS3 / PS4 / PS5 controller, the Xbox buttons on any other. F1 > Controls > Button prompts
  picks Automatic, Xbox, PlayStation or the original GameCube ones. The C-stick icon stays.
- **Control stick** (F1 > Controls): stick sensitivity (25-200%) and a round deadzone (0-50%,
  both sticks; the output grows from zero at its edge, so small aiming moves are smooth), with a
  meter showing the stick and what the game gets.
- F11 or Alt+Enter toggles fullscreen; `--fullscreen` starts that way.
- **Graphics** (F1, DISPLAY): internal resolution up to 4K, 4x MSAA anti-aliasing (from the next
  start; WebGPU offers no other sample count) and anisotropic filtering up to 16x.
- **Where your files are**: on Windows a `user` folder next to `red_two.exe` (the zip has one)
  holds settings, the memory card, save states, logs, texture packs, codes and the shader cache,
  so the folder can live anywhere (another drive, a NAS). An install that has no `user` folder
  but an older `%APPDATA%\Red Two` keeps using that one; create `user` to make it portable (its
  first start copies the settings, controls and memory card over). Linux: `~/.local/share/Red Two/`;
  macOS: `~/Library/Application Support/Red Two/`.
- **Cheats** (F1 > Cheats): invincibility, infinite lives, infinite bombs, perfect mission
  stats (the results screen says 0:01, every enemy destroyed, 100% accuracy, no friendlies or
  lives lost and 100% targeting computer efficiency, and the medal is awarded for those), and
  all craft (every craft a mission allows waits in its hangar). **Passcodes**: the game's own
  unlocks without typing codes - all missions, all tech upgrades, ace mode, art gallery, music
  hall, documentary, audio commentary, credits, black and white - kept in the game's save as
  if the codes had been entered.
- **RetroAchievements** (F1 > Achievements): log in with your retroachievements.org account
  (only the login token is stored) or take Dolphin's login; softcore only, so save states and
  cheats stay usable. Unlocks pop up in the top right corner.
- **Reset game**, **Back to launcher** and **Quit to desktop** are at the top of the menu's first page.
- **Save states**: Back + RB saves, Back + LB loads, Back + D-pad right / left picks the slot
  (8 slots; keyboard F5 / F8, F7 / F6). F1 > Controls has the slot, save / load buttons and the
  hotkeys, each any combination of buttons (or keys) held together: pick the row, press A, hold
  the buttons, let go. On a controller Back alone (released with nothing else pressed) still
  opens the menu. States live in the user folder's `states` and also load after a restart, but
  only into the same build of Red Two (a rebuild makes them "OLD"). The memory card is not part
  of a state: loading one does not undo the game's own saves.
- Saves go to the user folder's `saves/GSWE64_A.raw` (slot A; the game formats a new card). A
  `.raw` card from Dolphin or Nintendont can be copied there as it is, whatever its size (59 to
  2043 blocks). `R2_CARD_A=<path>` uses another card image.
- **Mods** (F1, MODS tab): texture packs in Dolphin's format go in the user folder's
  `textures\` (a `GSWE64` folder inside, exactly as in Dolphin's
  `Load\Textures`; `.dds` and `.png`, mip files and wildcards work), or stay wherever you unpacked
  them: MODS -> Texture pack folder opens a folder browser (d-pad / stick, A opens, X goes up, B
  back) - pick the pack's folder (the one holding `GSWE64`, or `GSWE64` itself), e.g. in Downloads
  or on an SD card. Back + D-pad up (F9) turns the texture packs off and on again at once, to
  compare (rebindable in Controls). Gecko / Action Replay codes and
  patches go in the user folder's `codes.ini` (Dolphin's GameSettings format) and can be switched
  in the menu. The game is recompiled ahead of time, so codes that change its code (Gecko C2
  "insert ASM", writes into code) cannot work; the menu marks them as not supported.
- **Importing from Dolphin**: MODS -> IMPORT FROM DOLPHIN, or
  `red_two.exe --import-dolphin[=saves,textures,codes] --console`. Red Two finds Dolphin's user
  folder (AppData, Documents, a portable install, the registry, a custom Load folder in
  `Dolphin.ini`) and brings over this game's newest save (from GCI folders or `.raw` cards; the
  previous card is kept in `saves\backup`, and the game restarts), its texture packs (copied) and
  its codes (with their on/off states). `R2_DOLPHIN_DIR=<User folder>` points it elsewhere.
- **Shaders** (F1, DISPLAY -> PERFORMANCE): "Prepare shaders at start" (on) compiles every shader
  the game has used on this device before it starts; off, the game starts at once and objects may
  pop in while their shaders compile. "Ubershaders" (on; off on a Steam Deck's first start) draws
  not-yet-compiled objects with a generic shader instead of skipping them.
- `R2_NO_AUDIO=1` runs without sound. Every session writes a log to the user folder's `logs`
  (Linux: `~/.local/share/Red Two/logs`); `--console` shows it in a console window on Windows.

## Reporting a problem

Open an issue on the [Issues](../../issues/new/choose) page and pick **Bug report** (or **Feature
request** for an idea): the form asks for what helps. Please check the open issues first, and
report one problem per issue. What to have at hand:

- **Red Two's version**: under the menu's title (F1, or the controller's Back / Select), e.g.
  "VERSION 1.2". Newer versions may already fix it: the menu offers updates at the top of its
  first page.
- **What happened and where**: the mission (or menu, cutscene) and the moment, whether it happens
  every time, and how to get there.
- **The session log (required)**: a bug report needs the log of the play session where the
  problem happened. Every start of Red Two writes a new one, named by the date and time it
  started, so if you started Red Two again afterwards, the newest log is from that later start:
  pick the right one by its date and time, and drag it into the issue. It's in:
  - Windows: `user\logs` next to `red_two.exe` (an older install without a `user` folder:
    `%APPDATA%\Red Two\logs`)
  - Steam Deck / Linux: `~/.local/share/Red Two/logs`
  - macOS: `~/Library/Application Support/Red Two/logs`

  It lists your settings, the frame rate every 30 seconds, and what the game was doing in a
  freeze or a crash. It also shows folder paths, which can include your user name: edit those
  out if you like.
- **Your setup**: the controller (and whether Steam starts Red Two), your display and aspect
  ratio, a texture pack if you use one.
- **A screenshot or a short video** for anything you can see.

A save state from just before the problem can help too, but states only load into the build that
made them: mention that you have one, and we'll ask for it if needed.

## How it works

`tools/rvz.py` and `src/common/disc_image.cpp` read the disc; the recompiler (`src/recompiler`)
finds the game's functions in `main.dol` without symbols and writes them out as C++ - including the
second executable the game hides inside `main.dol` and runs from its own demand-paged virtual
memory. The runtime (`src/runtime`) stands in for the console: the CPU state and MMU, threads on
host fibers, the GameCube's hardware at the register level (video, disc, audio DSP, ARAM, memory
card, controllers), and a bridge that hands the GPU command stream to
[Aurora](https://github.com/encounter/aurora) for rendering. Names we gave the game's functions are
in `config/GSWE64/symbols.txt`.

## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only, not the game's code that a build (and the prebuilt downloads) contain in recompiled form.

This repository contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
