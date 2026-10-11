# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- Streets of SimCity lifted whole with pcrecomp (22,308 functions, 0 lift
  errors, 6.6M lines of C) and run on the `native32` host. It plays the intro,
  goes through the main menu, the career's name dialog and mission list and
  the garage, and drives the first mission's city on the software renderer,
  the same as the shipping code with the same input. Needs pcrecomp
  `feat/iso-peek-joliet` (pcrecomp#92) and `fix/lift32-loopcc-zf` (pcrecomp#93).
- `tools/gamefiles.py` (the game off the CD or an ISO, by its Joliet names),
  `tools/catalog.py`, `run_lift.py`, the host (`src/runtime/`), `Setup.cmd`
  (Quick start), `tools/conformance.py` and a netlab recipe.
- The game folder is offered as the game's CD: a drive letter mapped onto it
  for the session, reported as a full CD-ROM.
- The host's DirectDraw (from Monster Truck Madness 2) with a clipper, primary
  blits taken relative to the window, the system palette, and the window's DC
  as the screen, so the Smacker videos SMACKW32 draws there are seen and
  recorded.
- `--original`: the shipping machine code under the same host, for comparing
  a lifted run against (from Tiberian Sun's `oracle.c`).
- Glide's board query answered "no boards", so the software renderer is
  picked without a `glide2x.dll`.
- `run_lift.py`: the rasterizer's "return to the caller's caller"
  (`pop eax; ret` and `pop eax; popal; ret`, 44 sites), a patch for the
  guarded divide at `0x004C7993` (the catch's path when the original's
  divide would fault), the one-time init routine at `0x004D9840` compiled
  unoptimised (its chunk took 40 minutes), and only changed chunks rewritten.
- `--original --lifted VA,...`: bisect a lift bug inside the original.
- `tools/catalog.py`: the code-pointer tables in `.data` seeded, so the
  rasterizer's unrolled loops are lifted.
- `--open FILE`: answers the game's file dialog unseen, so Player's Choice
  takes a SimCity 2000 city headless. Checked with cities from the SimCity
  2000 CD and cities saved by the SimCity 2000 recomp; all drive.
- A `SendMessageA(HWND_BROADCAST)` (the game's `WM_FONTCHANGE` at start) skips
  hung windows: a suspended Settings window kept the game from starting.
- Scripted keys answer `GetAsyncKeyState` as well as `GetKeyState`.
