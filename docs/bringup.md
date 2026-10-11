# Bring-up

Each wall between "it lifts" and "it plays", in the order they came, with how
it was found. The design that came out of them is in
[architecture.md](architecture.md).

All runs are headless, recorded, and scripted (REPO_RULES section 13):

```
build-farm\ssc.exe --run --headless --watchdog 80 --record scratch\run.mp4 ^
    --key ESC@6000 --click 225,95@12000 --key S@15000 --key ENTER@16000 ^
    --click 312,333@20000 --click 100,256@28000
```

That skips the intro, picks New Game, types a name, says yes to restarting a
career of that name if one exists, and picks the first mission, which opens
the garage.

## 1. The CD

```
[messagebox] Streets of SimCity: Please insert the Streets of SimCity CD-ROM or select 'Cancel' to quit Streets of SimCity.
```

over and over. `--native-trace` showed the check: `GetLogicalDriveStringsA`,
then `GetDriveTypeA` on each drive, looking for `DRIVE_CDROM` (5), from
`sub_00531F50`. With a drive letter mapped onto `game\` and reported as a
CD-ROM, the next call in the trace was

```
[native] KERNEL32.dll!GetDiskFreeSpaceA   (...) from sub_00513120 a0="w:\" -> 00000001
```

and the prompt again: a pressed disc has no free space. Answering 0 free
clusters for that letter got past it. The data is all there, so nothing on
the CD side needed redirecting.

## 2. DirectDraw: a clipper

```
[ddraw] CreateClipper: not implemented
[messagebox] Streets of SimCity: Streets of SimCity was unable to initialize DirectDraw and cannot run.
```

Monster Truck Madness 2's DirectDraw never needed a clipper. One that only
remembers its window is enough here.

## 3. The intro: black, then colour

The intro ran (107 frames in 8 s, `SmackWait`/`SmackDoFrame` in the trace)
and the recording was black. SMACKW32 imports are by ordinal; its exports
named them (`_SmackBufferBlit@32` and so on). The game wraps each
`SmackBufferBlit` in `GetDC`/`ReleaseDC` of its window, but a temporary hook
on SMACKW32's own imports showed where the picture really went:

```
[diag] smack GetDC(00C70CC2) -> 98011A15
[diag] StretchDIBits(98011A15 31,26 436x324 bpp 8 usage 1) -> 324
```

SMACKW32 gets its own DC of the window and draws with `DIB_PAL_COLORS`. The
host now points those imports at its screen (architecture.md, "The window's DC
is the screen"). The frames arrived, still black: the palette indices
resolved through an empty palette. SMACKW32 realizes the video's palette
itself, with its own `RealizePalette` import; hooking that one too gave the
intro its colours.

## 4. The menus: black, twice

After the intro, 5,000 presents a second, all black. Two causes, one under
the other:

- The DirectDraw palette was built from `GetSystemPaletteEntries`
  (`sub_0053A0D0`), which on a 32-bit desktop returns 0 entries. The host now
  answers with the palette GDI last realized.
- Still black. A probe on the blits showed full source surfaces and an empty
  primary:

  ```
  [ddraw] probe: Blt 017D8558 (640x480, dst 017D8510) src has 307200 nonzero, rect -1,-1--1,-1 dst 648,331-1288,811
  ```

  The game blits to the primary at its window's screen position (from
  `ClientToScreen`), which on the hardware it was written for was 0,0. The
  host's window was at 648,331, so every blit was clipped away. Destinations
  on the primary are now taken relative to the client area.

The main menu, the name dialog and the career's mission list all draw from
there, and the scripted clicks and keys work on them.

## 5. The rasterizer

Picking a mission opens the garage, which shows the car turning in 3D: the
first frame of the software renderer. It faulted there:

```
=== fault 0xC0000005 at 0x6198D1C0 ===
  write of 0x17BC4980
  in lifted sub_004B7A78, last native call (none)
```

`tools/addr2line.py` put it on `mov byte ptr [edi], ah` at `0x004B7CBE`, in a
hand-written texture-span loop. Three separate causes, found in this order:

1. **`loopne` tested CF.** The span loop ends `inc edi; loopne`. lift32 lifted
   `loope`/`loopne` as `--ecx != 0 && _cf == 0`; they test ZF. Spans ended on
   whatever carry a `shr` had left. Fixed in the toolkit (pcrecomp
   `fix/lift32-loopcc-zf`, with `tools/lift/test_loopcc.py`). The fault stayed.
2. **A call to 0.** `--original` (the shipping code under the same host) was
   added to compare against, and the original faulted first, at address 0,
   called from `0x004707D5`: `jmp [0x00C53BD4]`, `glide2x.dll!_grSstQueryBoards@4`,
   the 3dfx board query every start makes. Unbound, the original jumped to 0,
   and the lifted run skipped the call but left its stdcall argument on the
   stack. The host now answers it ("no boards") and `grGlideInit`. With that,
   the original drove the same script into the garage, car turning.
3. **Returning to the caller's caller.** The lifted run still faulted at the
   same write. The edge routines the span routine calls (`0x004B7CD0`,
   `0x004B7DBE`) end a polygon with

   ```
   004B7CE2  je 0x4b7dbc
   ...
   004B7DBB  ret
   004B7DBC  pop eax        ; drop our own return address
   004B7DBD  ret            ; and return to whoever called the span routine
   ```

   The lift returned to the span routine instead, which kept drawing past the
   last edge. `run_lift.py` now finds every `ret; pop r32; ret` (42, all in
   the rasterizer), sets a flag at the `pop`, and returns from any caller
   that sees it set after its call. The garage then matched the original.

Leaving the garage for the city faulted in a different family:

```
=== fault 0xC0000005 at 0x618BC5B0 ===
  read of 0xFFFFFFFF
  in lifted sub_004AB7B0, last native call (none)
  eax=1F2E9B31 ecx=8F0C0000 edx=C1000000 ebx=00001F15 esp=1092FD7C ebp=00000004 ...
last indirect calls (newest first):
  0x004C446A  from 0x004C3890
```

`ebp = 4` in an ordinary C function: its frame was gone. `0x004C446A` was not
in the dispatch table. The span routine at `0x004C3890` ends
`jmp [ebp*4 + 0x00688F4C]`, a table of 32 entry points into an unrolled loop
just after it, in initialised `.data`. Nothing branches to them, so the
catalog never reached the loop and the jump went nowhere. `.data` holds 30
such tables for the rasterizer. `tools/catalog.py` now seeds every run of 8
or more `.data` dwords that point into `.text`: 1,127 addresses.

## 6. The city

With the tables seeded, leaving the garage faulted again, in one of the
seeded entries:

```
=== fault 0xC0000005 at 0x6197FFDA ===
  read of 0x07393121
  in lifted sub_004B5F4D, last native call (none)
```

`--original` was the next step, and it died on the way into the city with an
exception of its own:

```
[original] exception 0xC0000095 at 004C7993 (read 0x00000000) ...
```

`0x004C7960` divides inside a C++ `try { } catch (...)`; when the quotient
does not fit, the CPU raises `STATUS_INTEGER_OVERFLOW` and the catch sets an
overflow flag (`0x00B27A58`), logs "Caught Exception in Divide" and stores
`1:0`. The original died because Windows will not run an exception handler
that lives outside a loaded image in a DEP process, and `oracle.c` maps
`Streets.exe` itself. Its vectored handler now does what that catch does. The
lifted `idiv` raises nothing at all, so `run_lift.py` PATCHES decide whether
the original's would have, and take the catch's path when it would.

The original then drove into the city: down the street, through a tunnel,
into a wall, with the throttle held. The lift still faulted, so the next step
was to bisect: `--original --lifted 4AB7B0` runs the original with just the
polygon routine (and everything it calls) lifted, through native32's
`lift_in_native`. It faulted the same way, so the bug was in the rasterizer.
The span routine that owns `0x004B5F4D` opens with `pushal`, and its edge
routines end differently from the first family's:

```
004B6147  ret
004B6148  pop eax        ; drop our own return address
004B6149  popal          ; restore the span routine's registers for it
004B614A  ret            ; and return from the span routine
```

`run_lift.py` took only `ret; pop r32; ret`. It now takes `ret; pop r32`
followed by any pops, `popal` or `popfd`, then `ret`: 44 sites. It looks only
inside the rasterizer (`0x004B2000`-`0x004C7000`), since in compiled code a
`ret` followed by a jump-target epilogue is an ordinary second exit. With
that, the lift drives the same route as the original, frame for frame, and
the conformance run passes 12 of 12.

### Not done: a second guarded divide

`0x004AB0FB` is a fixed-point divide with no `try` of its own; when it
overflows, the exception unwinds to a `catch (...)` in the game-state
function `0x0042D010` (FuncInfo `0x005D3A98`, catches at `0x0042D1D7` and
`0x0042D446`), which runs error recovery for the frame. The lifted `idiv`
just carries on with a truncated quotient. Nothing visible has come of it in
the scripted runs, but it is not what the original does. The general fix is
in the toolkit: a lifted `idiv` or `div` that would fault raises through
eh32 (the guest's own C++ exception dispatch), where a `catch (...)` takes it
as the original's did. Until then `--original` dies when it hits this one
(it does, on some runs, on the way into the city): the host emulates only the
first catch.
