#!/usr/bin/env python3
r"""Put your copy of Streets of SimCity's files in game/, from the disc.

    py -3 tools/gamefiles.py StreetsOfSimCity.iso     # a 2048-byte ISO
    py -3 tools/gamefiles.py E:\                      # the CD in a drive, or a copy of it

The disc is plain files, no cabinets. The game is EXE\ (Streets.exe,
SMACKW32.DLL, TILES.DB, the SCURK city editor); its data is in folders at the
root (BMP, Cities, FONTS, GEO, Games, SMK, SOUND, Weapons, X, scenarios,
HELP), opened by relative path from the game's folder. So game/ is EXE\'s
files with the data folders beside them, the layout of a full install.
Setup, DirectX 5, Acrobat, the registration and online tools are not copied.

Disc images are read with pcrecomp's ISO9660 walker
(tools/assets/iso_peek.py), so nothing is mounted. It supplies your own copy
to the local build; nothing is downloaded, and none of it is ever committed.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'game')
EMPTY = ('Games/Careers', 'Games/Used Cars', 'SOUND/RADIO/STATIONS/MIX/MUSIC')
DATA = ('BMP', 'CITIES', 'FONTS', 'GEO', 'GAMES', 'SMK', 'SOUND', 'WEAPONS', 'X', 'SCENARIOS', 'HELP')


def target(p):
    """Where disc path p goes under game/, or None."""
    top, _, rest = p.partition('/')
    if not rest:
        return None
    if top.upper() == 'EXE':
        return rest
    return p if top.upper() in DATA else None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    if os.path.isdir(src):
        files = [(os.path.relpath(os.path.join(d, f), src).replace('\\', '/'), os.path.join(d, f))
                 for d, _, fs in os.walk(src) for f in fs]
        reader = None
    else:
        sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'pcrecomp')),
                                        'tools', 'assets'))
        import iso_peek
        reader = iso_peek.Reader(src)
        files = [(p, (off, n)) for p, off, n in iso_peek.walk(reader)]
    # Folders the disc ships empty, which the game saves into (careers, cars).
    for d in EMPTY:
        os.makedirs(os.path.join(OUT, *d.split('/')), exist_ok=True)
    n = 0
    for p, where in files:
        rel = target(p)
        if rel is None:
            continue
        dst = os.path.join(OUT, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if reader is None:
            shutil.copyfile(where, dst)
        else:
            off, size = where
            with open(dst, 'wb') as f:
                f.write(reader.read(off, size))
        n += 1
    if not os.path.exists(os.path.join(OUT, 'Streets.exe')):
        sys.exit('no EXE\\Streets.exe in %s: is this the Streets of SimCity disc?' % src)
    print('%d files in %s' % (n, OUT))


if __name__ == '__main__':
    main()
