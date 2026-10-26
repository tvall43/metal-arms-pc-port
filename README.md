# Metal Arms - Windows port

A Windows build of *Metal Arms: Glitch in the System* (Swingin' Ape, 2003),
rebuilt from the original 1.19.1 source drop (`App`, `Lib`, `Exp`, `Plug`,
`Ref`, `TidBits`) and finished with the fixes the port needed: modern input,
widescreen, portable saves, and a number of gameplay/parity repairs verified
against the retail Xbox build (`default.xbe`).

Current port round: **R133** (`app/ma/win/main_win.cpp`, `MA_PORT_ROUND`).

## What this is (and is not)

- **Single-player campaign, complete.** All levels, checkpoints, saves,
  vendors/EUK upgrades, movies and credits work end to end.
- **Multiplayer works.** The versus modes were sorted out early in the port
  and stay part of it - grab extra pads. Secret chips collected in the
  campaign unlock the locked multiplayer arenas, and beating a level's
  posted time earns its Speed Chip.
- **Retail Xbox data is not included.** This tree compiles to `ma_win.exe`,
  but at runtime it needs the game data from an original Xbox disc. You must
  supply your own copy; nothing retail (`default.xbe`, master file, movies,
  sounds, textures) ships with the repo or any release zip.
- **The source drop is copyrighted.** The code in this repository belongs to
  the game's original authors and publisher. This port exists for personal and
  research use. Do not redistribute the tree or a build that includes the
  leaked source; keep any hosting private.

## Running the game

The exe is portable - it creates its data next to itself and never writes
outside its own folder:

```
MetalArmsPC/
  ma_win.exe          port build
  ma_win.ini          created on first run; settings only
  binkw32.dll         ships with the port (RAD movie decoder; loaded by name)
  assets/             your own extracted Xbox disc contents
    mettlearms_xb.mst master file (auto-detected here)
    MOVIES/ ...
  saves/              profile files (created automatically: saves/profile-*)
  ma_win_log.txt      only when debug logging is enabled
```

First run:

1. Unzip the release (or drop in a locally built `ma_win.exe`). `binkw32.dll`,
   the RAD movie decoder, is already included in the release - it just needs to
   stay next to `ma_win.exe`. There is nothing to download or source yourself;
   if you move `ma_win.exe` somewhere, move `binkw32.dll` along with it.
2. Launch `ma_win.exe` and use **Extract from ISO...** in the launcher to read
   the game out of your own Xbox disc image (both game-only `.xiso` images and
   full-disc `.iso` images are supported; the extraction also sets the master
   file for you). Alternatively, place your already-extracted disc contents in
   `assets/` so that `mettlearms_xb.mst` sits at its top.
3. Press play. Profiles live in `saves/profile-*`; an old flat `profile-*` next
   to the exe is migrated into `saves/` on first start.

`ma_win.ini` lookup order for the master file: `assets/mettlearms_xb.mst`
first, then the stored `[Paths] MasterFile`, then the old flat
`<exe>/mettlearms_xb.mst` for installs made before the portable layout.

### Launch modes (command line, case-insensitive, leading `-` or `/` optional)

| Token | Behavior |
|---|---|
| *(none)* | Retail-mode launch dialog (dev rows hidden, no debug log) |
| `play` | Boot straight into the campaign in retail mode |
| `dev` | Development mode: boot-mode chooser, dev menu, dev cheats, Level jump on the launch dialog |
| `autorun` | Development mode, boot straight in (the original vcproj behavior) |

Retail mode never writes a log file, ignores the dev-only debug options, and
starts clean. A VectoredExceptionHandler crash logger is always active and
writes a backtrace next to the exe on a hard crash.

Public releases ship a stripped `ma_win.exe` (about 7 MB; the unstripped build
is 70 MB because of DWARF). The debug information for each shipped build is
archived as `ma_win_<build>.dbg` beside the release zip; binaries are not
reproducible, so that file is the only way back from a crash address:

```
addr2line -f -e ma_win_r137.dbg 0x<address from the crash log>
```

Stripping does not move code, so addresses in the shipped exe and the `.dbg`
are identical.

### Reporting a problem

The build logs everything it might need; a report without the log usually
cannot be diagnosed. Run `ma_win.exe dev` and tick **Debug log file** on the
launch dialog, play until it happens again in one session if you can, then
quit and attach `ma_win_log.txt` from the game folder. Crashes write their own
backtrace next to the exe with no flags. File a GitHub issue rather than a
comment on a download page: issues allow replies and detail, page comments
cannot be replied to. Name the build id printed in the log header (e.g. R137), and
check what version you have first - fixes land continuously.

## Configuration (`ma_win.ini`)

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Video]` | `Width` / `Height` / `ColorBits` | 640 / 480 / 32 | Present backbuffer size |
| `[Video]` | `Fullscreen` | 0 | 1 = fullscreen |
| `[Video]` | `MovieScaling` | `fit` | Bink frame scaler: `fit` (aspect-locked, pillars where needed), `fill`, `stretch` |
| `[Audio]` | `Volume2D`, `MinDistanceFraction` | engine defaults | Speech/UI volume and 3D sound falloff start |
| `[Mouse]` | `Sensitivity` | 1.0 | Mouse-look scale |
| `[KEYMAP]` | `Jump`, `SelectPrimary`, `SelectSecondary`, `Action`, `Start`, `Back`, `Fire`, `Aim`, `Black`, `White`, `Crouch`, `Melee` | default bindings | Keyboard overrides for the pad action layer; buttons only (WASD / IJKL / arrows are fixed), unknown key names fall back to the default |
| `[Paths]` | `MasterFile`, `Level`, `ScreenshotDir` | auto | Advanced; `MasterFile` is normally not needed |
| `[Debug]` | `LogFile`, `TempProbes`, `WireLightForce`, `WireCullNone` | off | Development-only diagnostics; retail mode ignores them |

The 3D world renders wide; UI/letterbox content stays in the authored 4:3
canvas centred on screen.

## Controls

- **Gamepads:** XInput (Xbox-family) controllers are the primary path;
  DirectInput pads still enumerate. Layout matches the retail Xbox pad
  (stick move/look, triggers fire/aim, Black/White quick-select, etc.).
- **Keyboard and mouse:** gameplay reads the same action layer, with mouse
  look built in - there is no separate keyboard/mouse consumer path.
- Keyboard overrides per mapping go through `[KEYMAP]` (key names like `Tab`,
  `Shift`, `Q`, `E`, ... unknown names fall back to the default binding).

## Testing

- The full single-player campaign has been completed end to end on **Linux
  under Wine** (D3D8 through WineD3D): every level, shop, movie and
  checkpoint in order.
- On **Windows**, what got exercised is the versus multiplayer over four
  Xbox One pads. The Windows build is the same code through native D3D8, so
  the campaign is expected to behave the way it does under Wine - it just
  has not been proven end to end there yet.

## Building

The tree builds as a 32-bit Windows graphical exe with mingw-w64. It refuses
64-bit at CMake time on purpose: the code was written for a 2002-era 32-bit
ABI (32-bit `long`, 4-byte pointers) and is layout-sensitive at class
granularity, so a 64-bit build would corrupt struct layouts silently.

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake
cmake --build build -j8
```

- CMake >= 3.20, a 32-bit mingw-w64 toolchain (i686-w64-mingw32), C++11.
- The original `*.vcproj`/`make_ma*.bat` Visual Studio .NET 2003 projects are
  kept for reference; the MFC debug shell (`MA_BUILD_SHELL`) is not buildable
  here and is excluded.
- The engine also served the GameCube/PS2 builds of the game (the PPC-era
  code paths are still in the tree under `FANG_PLATFORM_GC`/`_PS2`); the
  Windows build itself uses the Xbox layout rules - 16-byte alignment on the
  `FCLASS_ALIGN`-tagged classes, matching retail Xbox data.
- The `_FANGDEF_WINGC` switch (`MA_WINGC`) toggles the engine's GameCube
  compatibility mode on a Windows build: it disables the SSE (`__m128`) paths
  in the vector types and activates the `#if FANG_WINGC` code branches, which
  is where this port's gameplay parity fixes live. It must agree across every
  translation unit - the original vcprojs built some libraries with it and
  others without, which silently mismatched struct layouts. This port builds
  with `MA_WINGC=ON`; the retail-provenance leak paths remain in the `#else`.
- The engine (`Lib/Fang2/dx`) drives D3D8 directly. On modern Windows the game
  runs fine through any D3D8-capable path; on Linux/Wine it runs WineD3D.

## Repository layout

```
App/ma        game code (bots, weapons, levels, HUD, scripts)
App/ma/win    the Windows shell: entry point, launcher, input glue, config
Lib/Fang2     Fang engine (render, D3D8 driver, audio, physics, world/coll)
Exp, Plug     exporters and editor plug-ins from the original drop
Ref           reference material from the original drop
TidBits       bits and pieces from the original drop
cmake         toolchain + build scripts (mingw-w64 i686)
compat        compatibility shims added by the port
```

## Port notes

Heritage: this tree is the original 1.19.1 Swingin' Ape source, compiled
straight through a 32-bit mingw-w64 toolchain into a modern Windows exe.

How it was made: this is a mainly AI-written port. The work was driven
through the OpenCode agent harness in a long series of sessions using a
mixture of mimo-v2.6-flash, deepseek-v4.1-flash and glm-5.3-flash builds,
with a human steering direction and playtesting/verifying every round's
build before it shipped.

The port treats the retail Xbox build (`default.xbe`) as the source of truth:
gameplay fixes are verified against its code/data (addresses in the archive
notes) before shipping, and port-side changes are tagged under
`#if FANG_WINGC` (defined by this build) so the leak-provenance `#else` paths
stay intact. The work so far, in the order it mattered:

- **Boot, render, play.** Got the engine cross-compiling and the retail
  master file loading without the old toolchain; fixed the GCC-vs-MSVC
  anonymous-struct degeneracy that silently broke the engine's transform math
  (the first reason nothing on screen was trustworthy).
- **Renderer archaeology.** The PC paths had been hollowed out along the
  way: liquid rendering (water/lava/ooze) existed only as `#if`-guarded
  empties (three stacked faults: empty renderer bodies, a tick guard, an
  unregistered reflection render target); the cube-face water reflection pass
  and two missing pixel-shader table entries had to be re-ported; invisible
  flat geometry (fences, wires, cuttable floors) came from a stale
  prebuilt `ps.1.1` passthrough whose baked-in alpha multiply reduced every
  translucent draw to nothing; vertex-lit / lightmapped passes plus
  particle/glow/locked-on icons were re-derived from the retail image.
- **Dynamic-object lighting.** Destructible and dynamic objects (pipes,
  bridge segments, debris, bots, weapon models) rendered as if fully lit in
  correctly dark scenes, flipping between correct and over-lit as the camera
  moved. The lighting shaders compute point-light falloff as
  `1/(a0 + a1*d + a2*d^2)`; light passes that used fewer lights than slots left
  the unused attenuation rows at zero, so the division produced infinity and
  the shader's clamp turned the whole fill white. The reset now seeds every
  unused row with the neutral `(1,0,0)`.
- **Starting health.** A fresh profile started with three health batteries,
  where retail starts the first mission with one. The authored battery pickups
  are numbered 2 and 3, so both looked like already-owned repeats. New profiles
  now start with one; existing saves keep the count they carry.
- **Campaign plow through.** Level-load crash on EUK collectables beyond
  level 2, the never-appearing barter shops (retail table-name drift),
  "Unhandled Exception" load crash, the tether possession softlock, the rat
  chase's wrong-way drive (scalar XZ math), reactor cutscene hangs, arena
  wave spawns, end-of-level cutscenes and credits.
- **Combat parity.** Every difficulty setting was silently ignored (retail
  5-field CSV vs the leak's 2); debris groups, scout detonation, predator
  quad-laser spread, AA mortar rate, Sentinel cannon damage, flamer
  per-instance damage profiles (fixed possessed bots one-shotting runs),
  chip-grunt goodie acceptance, Scatter Blaster retail damage grouping (one
  representative pellet per entity, centrality x cubic-range falloff),
  washer attraction, arm-servo persistence across body spawns.
- **Audio.** Music, 3D world SFX and streamed bot dialog rebuilt end to end;
  retail weapon sound groups (the flamer was completely silent before), the
  2D attenuation ladder, stream-tail repetition, and two ini mix knobs.
- **Fog.** The 52 levels that set fog (depth + height bands, per level CSV) get
  it: destination-alpha back buffer, vertex-shader fog amounts, and the
  full-screen fog blend pass, matching the retail Xbox renderer.
- **Input.** XInput pads (Xbox-family controllers preferred), DirectInput
  fallback, raw mouse look with per-consumer behavior (driving, turrets,
  site weapons), rebindable pad actions via `[KEYMAP]`.
- **Packaging and running.** Retail mode by default with `-dev` for the
  development shell, `ma_win.ini` beside the exe, portable `assets/`/`saves/`
  layout with profile migration, built-in XDVDFS extractor behind the
  launcher's "Extract from ISO..." button, Bink movies scaled to any
  resolution (`fit`/`fill`/`stretch`), F12 screenshots, crash logger.

Known gaps: the jump-pad effect spreads around
the pad instead of centring on it, and a few other particle/glow effects are
off; movie playback uses the RAD `binkw32.dll`, which ships with the port next
to the exe - nothing to obtain separately.
