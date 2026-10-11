# Roadmap

## Next
- Guarded divides, in the toolkit: a lifted `idiv`/`div` that would fault
  should raise through eh32 to the guest's `catch (...)`, as the CPU did. The
  game's second one (`0x004AB114`) unwinds to the game-state function's
  catch; today the lift carries on with a truncated quotient
  (docs/bringup.md 6).
- Play a whole mission: steer with `--key`, finish a delivery, and a
  milestone for it.
- Play it windowed on a real screen, and fix what that shows (focus, sound,
  the radio). A windowed run on an offstage monitor starts and takes input;
  offstage's recording failed on this machine for any program, so there is
  no capture of it yet.
- Upstream the two toolkit changes (pcrecomp `feat/iso-peek-joliet`,
  `fix/lift32-loopcc-zf`) and pin the toolkit to a release that has them.
- Upstream the general mechanisms found here into pcrecomp, so the next game
  gets them for free:
  - disasm32: take code-pointer tables in `.data` as entries (what
    `tools/catalog.py` seeds by hand);
  - lift32: "return to the caller's caller" (`pop r32; ret` after the callee
    dropped its own return address), which needs stack-depth tracking to
    find in general (what `run_lift.py` does by shape);
  - native32: an unresolved indirect call should not leave a stdcall's
    arguments on the stack silently (the Glide query showed it).
- Compile time: a routine with many alias entries is lifted once per alias.
  `0x004D9840` (run once) is compiled unoptimised for now; lifting an alias as
  an entry into its host function would end that for every title.
- Saves and careers: check they load, and where they go (`game\Games\`).

## Later
- Network play (DirectPlay, Winsock).
- The OpenGL renderer, which needs nothing the host does not already pass
  through; the Glide one through a Glide wrapper.
- Higher resolutions. The software renderer draws 640x480; whether more is
  reachable is unknown.
- Native Linux, on pcrecomp `win32hle`, as Tiberian Sun has.

## Out of scope
- Shipping any part of the game, or anything lifted from it.
- The city editor (`WINSCURK.EXE`) and the race editor (`RaceEdit.exe`).
