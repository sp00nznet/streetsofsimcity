# Architecture

## The game

Streets of SimCity (Maxis, 1997) is one PE image, `Streets.exe` (3.4 MB, MSVC,
linker 4.20, built 1997-10-22, PE timestamp `0x344E75C0`). The C runtime is
linked in statically and there is no protection. It has three renderers:
software (DirectDraw, 640x480 in 8 bits), Glide (3dfx) and OpenGL. The software
one is the default and the only one this build uses so far.

| File | What it is |
|---|---|
| `Streets.exe` | The game: menus, the city, the cars, the network game, all three renderers |
| `SMACKW32.DLL` | RAD Smacker, for the `.smk` videos (the intro, the mission briefings). Runs natively |
| `TILES.DB`, `DEFAULT.SC2` | Tile data and the default city |
| `BMP\`, `FONTS\`, `GEO\`, `SOUND\`, `SMK\`, `X\` | Menus, fonts, 3D models, sound and radio, videos |
| `Cities\`, `scenarios\`, `Games\`, `Weapons\` | Cities, career missions, saves and cars, weapons |
| `WINSCURK.EXE`, `RaceEdit.exe` | The city editor and the race editor. Not lifted |

The disc is plain files. `EXE\` holds the programs; the data folders sit at
the root, and the game opens them by relative path from its own folder
(`Scenarios\%s\%s`, `bmp\tvMenu1.bmp`). `tools/gamefiles.py` puts `EXE\`'s
files and the data folders side by side in `game\`, the layout of a full
install.

## The pipeline

```
disc / ISO ── tools\gamefiles.py ── game\Streets.exe ── tools\catalog.py ── work\functions.json
              (pcrecomp iso_peek)                        (pcrecomp disasm32)          │
                                                                                run_lift.py
                                                                    (pcrecomp generate + lift32)
                                                                                      │
src\runtime\host.c + ddraw.c + present.c + oracle.c + pcrecomp native32 ── build ── src\recomp\gen\recomp_00NN.c
                                                                             │
                                                                       build\ssc.exe
```

1. **`tools/gamefiles.py`** reads the ISO with pcrecomp's `iso_peek`, from its
   Joliet tree: the disc's names are long and mixed-case (`selectCar.bmp`,
   `BMP\steer\comp+30.bmp`), and the plain ISO9660 tree has only 8.3 ones.
   It needs pcrecomp with Joliet support (`feat/iso-peek-joliet`, pcrecomp#92).
2. **`tools/catalog.py`** runs pcrecomp `disasm32`: 22,304 functions, 97.2% of
   `.text`. It seeds every code-pointer table in `.data` first (below, and
   [bringup.md](bringup.md) 5).
3. **`run_lift.py`** lifts every catalogued function into one dispatch table:
   22,308 functions, 6.6M lines of C, 0 lift errors. It writes only the chunks
   whose text changed, so a lifter fix recompiles three files, not 56.
4. **The build** compiles the lift, the host and pcrecomp's `native32` into
   one 32-bit exe linked at `0x60000000`, so the guest's `0x00400000` is free.
   Day to day it runs on netlab (`netlab build streetsofsimcity`, clang-cl); `build.cmd`
   is the MSVC fallback and the Quick start's route.

## At run time

`ssc.exe` maps `Streets.exe` at its own base, with its code non-executable,
and runs only lifted code. `native32` bridges guest calls to Windows, and
Windows callbacks (the window procedure, the sound threads) back into lifted
code. What the host changes about the game's world:

- **The game folder is the CD.** At every start the game walks
  `GetLogicalDriveStringsA` for a drive `GetDriveTypeA` calls a CD-ROM
  (`sub_00531F50`), then wants it full (`GetDiskFreeSpaceA`, `sub_00513120`),
  and asks for the disc until it finds one. The host maps a free drive letter
  onto `game\` for its logon session (`DefineDosDevice`, what `subst` does, no
  admin) and answers both questions for that letter as a pressed disc would:
  a CD-ROM with no free space. A letter a crashed run left mapped onto the
  same folder is reused; a clean exit removes it.
- **DirectDraw is the host's own** (`ddraw.c`, from Monster Truck Madness 2,
  itself Virtual Springfield's): surfaces in memory, and an exclusive
  640x480x8 mode that is only remembered. Three things this game needed:
  - *The primary is addressed in screen coordinates.* The game blits to it at
    its window's client area from `ClientToScreen`; its window sat at 0,0 of a
    640x480 desktop. The host's is wherever it is, so a destination on the
    primary is taken relative to the client area.
  - *The system palette.* The menus take their DirectDraw palette from
    `GetSystemPaletteEntries` (`sub_0053A0D0`). A 32-bit desktop has no
    system palette and answers 0 entries, so every menu was black. The host
    keeps the palette GDI last realized (the game realizes one for each
    video) and answers with that.
  - *A clipper.* Only remembered: a fullscreen primary overlaps nothing.
- **The window's DC is the screen.** SMACKW32 draws each video frame into
  `GetDC` of the window it was given, with `StretchDIBits(..., DIB_PAL_COLORS)`.
  On a cloaked window none of it is seen. The host points SMACKW32's own
  `GetDC`, `ReleaseDC`, `StretchDIBits` and `RealizePalette` imports at its
  screen: `GetDC` of the game's window is a memory DC over the host's 32-bit
  copy of the screen, palette indices are resolved through the realized
  palette, and `ReleaseDC` presents the frame. The game's own `GetDC` of its
  window gets the same DC.
- **No 3dfx board.** `Streets.exe` imports `glide2x.dll` statically and calls
  `grSstQueryBoards` at every start (`0x004707D5`). Without a Voodoo there is
  no `glide2x.dll`; the host answers `grGlideInit` and "no boards" itself, and
  the other 35 Glide imports stay unbound (they are called only once a board
  was found). Left unbound, the query was a call to 0, which the lifted run
  skipped without popping its stdcall argument, and the original faulted on.
- **Broadcasts skip hung windows.** The game's `WM_FONTCHANGE` broadcast at
  start goes out with `SendMessageTimeoutA(SMTO_ABORTIFHUNG)`: a plain
  `SendMessageA` to `HWND_BROADCAST` waits on every window of the desktop.
- **Headless** (`--headless`, REPO_RULES section 13): every top-level window
  is cloaked, nothing takes the focus, and `MessageBoxA` goes to stderr.
  `--record` pipes the host's screen to ffmpeg at 30 fps against the wall
  clock. `--open FILE` answers the next
  `GetOpenFileNameA` (Player's Choice's `.sc2`) unseen. `--key` and `--click` script the keyboard and mouse: keys are posted
  to the focus window and held for `GetKeyState` and `GetAsyncKeyState`
  (the game polls both while driving).

### The rasterizer

The software renderer's span routines (`0x004B2xxx`-`0x004C6xxx`) are
hand-written assembly, and do three things the lift had to be taught; each is
in [bringup.md](bringup.md):

- Their edge-walking helpers end a polygon with `pop eax; ret` (or
  `pop eax; popal; ret`, restoring the span routine's registers too): they
  drop their own return address and return from whoever called them.
  `run_lift.py` finds the 44 of them by shape (`ret; pop r32`, any pops,
  `popal` or `popfd`, `ret`, inside the rasterizer only), and lifts each such
  `pop` with a flag set; every direct call to a routine that holds one is
  followed by a check that returns at once. That is the lift of "return to
  the caller's caller".
- They draw through unrolled loops entered part-way, `jmp [reg*4 + table]`,
  with the 32 entry addresses in initialised `.data`. `tools/catalog.py`
  seeds every run of 8 or more `.data` dwords pointing into `.text`
  (1,127 addresses), so the loops are catalogued, lifted and dispatchable.
- Their span loops end `inc edi; loopne`. lift32 tested CF for
  `loopne`/`loope`; it tests ZF now (pcrecomp `fix/lift32-loopcc-zf`, #93).

### Guarded divides

The game divides inside C++ `try { } catch (...)` blocks and relies on the
CPU's divide-overflow exception reaching the catch. A lifted `idiv` raises
nothing. The one at `0x004C7993` (its catch sets the renderer's overflow flag
at `0x00B27A58`) is patched in `run_lift.py` (PATCHES) to take the catch's
path when the original's divide would have faulted. A second, `0x004AB114`,
unwinds to the game-state function's catch and is not handled yet
([bringup.md](bringup.md) 6; the general fix is a toolkit one).

### --original

`--original` runs the shipping machine code instead of the lift, under the
same host: the same shims, display, CD and input (`oracle.c`, from Tiberian
Sun's, itself civ3's). It is the reference a lifted run is compared against;
the rasterizer's bugs were found by running the same script both ways.
`--original --lifted VA,...` runs those functions (and what they call) lifted
inside the original, through native32's `lift_in_native`: a lift bug is
bisected down to a function that way.

Windows will not run an exception handler outside a loaded image in a DEP
process, and the oracle maps `Streets.exe` itself, so the game's own catch
blocks never run under `--original`. The host emulates the guarded divide at
`0x004C7993`; the one at `0x004AB114` ends the run when it overflows.

## The presenter

Windowed (not headless), `present.c` draws the 640x480 picture into the game's
window with Direct3D 11, scaled. The window's Video menu picks the filter
(sharp-bilinear by default; smooth, nearest, integer, Scale2x), the window
size and a CRT look; F11 or Alt+Enter is fullscreen, and `ssc.ini` beside
`ssc.exe` remembers them. It is Monster Truck Madness 2's presenter unchanged
(itself Hover!'s and gunman's); mouse positions are mapped back to 640x480.
In fullscreen, Esc leaves fullscreen rather than reaching the game.
