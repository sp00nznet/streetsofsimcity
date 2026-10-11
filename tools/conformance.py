#!/usr/bin/env python3
"""Streets of SimCity conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Milestones**: one scripted headless run of build/ssc.exe (the script in
  docs/bringup.md: skip the intro, New Game, a name, the first mission,
  then leave the garage and hold the throttle), scored by the lines the host
  prints and by the recording. `--original` runs the same script on the
  shipping code, the reference; it is not scored, and it can stop early: a
  divide overflow the game catches with catch(...) and that the host does not
  emulate (0x004AB114) ends it (docs/bringup.md 6).
* **Lift health**: from the generated tree. Lift errors, bodies with no
  terminator, and RECOMP_ITAIL labels that cannot resolve at run time (their
  target is not in the dispatch table).

The game is not in the repo. Without game/ and a build this prints SKIP and
exits 0, so it can sit in CI and on netlab's test VMs without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
    py -3 tools/conformance.py --original   # the same script on the shipping code
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen')
BASELINE = os.path.join(ROOT, 'conformance.json')
VIDEO = os.path.join(ROOT, 'work', 'conformance.mp4')
SECONDS = 90
SCRIPT = ['--key', 'ESC@6000', '--click', '225,95@12000', '--key', 'S@15000', '--key', 'ENTER@16000',
          '--click', '312,333@20000', '--click', '100,256@28000', '--click', '568,437@45000',
          '--key', 'UP@62000+25000']


def host():
    """SSC_EXE, else the newest of build/ and build-farm/."""
    if os.environ.get('SSC_EXE'):
        return os.environ['SSC_EXE']
    have = [p for p in (os.path.join(ROOT, d, 'ssc.exe') for d in ('build', 'build-farm')) if os.path.exists(p)]
    return max(have, key=os.path.getmtime) if have else None


# (name, a regex the host prints, a time in seconds at which the recorded
# screen must not be black, or two times between which it must change).
# Order is the script's order.
MILESTONES = [
    ('image mapped and imports bound', r'mapped Streets\.exe|running the original machine code'),
    ('the game folder is the CD', r'\[cd\] \w:\\ is the game folder'),
    ('no CD prompt', None),
    ('DirectDraw created', r'DirectDrawCreate -> host DirectDraw'),
    ('the intro plays (100 video frames)', r'frame 100 presented \(video\)'),
    ('the intro is not black', 4.0),
    ('main menu drawn', 10.0),
    ('mission list drawn', 26.0),
    ('garage drawn (3D car, software renderer)', 40.0),
    ('the city drawn (software renderer)', 60.0),
    ('driving: the view moves with the throttle held', (64.0, 80.0)),
    ('no fault', None),
]


def frame(t):
    """The recording at t seconds, 64x48 grey, as bytes (empty if none)."""
    return subprocess.run(['ffmpeg', '-loglevel', 'error', '-ss', str(t), '-i', VIDEO, '-frames:v', '1',
                           '-vf', 'scale=64:48', '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                          capture_output=True).stdout


def lit(t):
    """Mean brightness (0-255) of the recording at t seconds, or -1."""
    f = frame(t)
    return sum(f) / len(f) if f else -1


def moved(a, b):
    """Did a quarter of the picture change between a and b seconds?"""
    fa, fb = frame(a), frame(b)
    return bool(fa) and len(fa) == len(fb) and sum(abs(x - y) > 16 for x, y in zip(fa, fb)) > len(fa) // 4


def run(exe, original):
    cmd = [exe, '--run', '--headless', '--watchdog', str(SECONDS), '--record', VIDEO] + SCRIPT
    if original:
        cmd.append('--original')
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=SECONDS + 60)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
    passed = []
    for name, test in MILESTONES:
        if name == 'no CD prompt':
            ok = 'insert the Streets of SimCity CD-ROM' not in out
        elif name == 'no fault':
            ok = '=== fault' not in out
        elif isinstance(test, float):
            ok = lit(test) > 8
        elif isinstance(test, tuple):
            ok = moved(*test)
        else:
            ok = re.search(test, out) is not None
        if ok:
            passed.append(name)
    last = [l for l in out.splitlines() if l.startswith(('===', '[watchdog]', '  in lifted'))]
    return passed, last[:3]


def lift_health():
    stats = json.load(open(os.path.join(ROOT, 'work', 'lift_stats.json')))
    disp = set(re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(GEN, 'recomp_dispatch.c')).read()))
    unresolved = sum(1 for fn in glob.glob(os.path.join(GEN, 'recomp_0*.c'))
                     for t in re.findall(r'L_([0-9A-F]{8}): RECOMP_ITAIL', open(fn).read())
                     if t not in disp)
    return {'lifted': stats['lifted'], 'errors': stats['errors'],
            'no_terminator': stats['no_terminator'], 'unresolved_itail': unresolved}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--original', action='store_true', help='run the shipping code instead (no baseline)')
    ap.add_argument('--exe', help='the host to test (netlab passes its build); default SSC_EXE, else the newest build')
    args = ap.parse_args()
    exe = args.exe or host()
    if not (exe and os.path.isdir(os.path.join(ROOT, 'game')) and os.path.exists(os.path.join(GEN, 'recomp_dispatch.c'))):
        print('conformance: SKIP -- needs game/ (your copy), the lift and a build (README, Step by step)')
        return 0
    os.makedirs(os.path.dirname(VIDEO), exist_ok=True)

    passed, last = run(exe, args.original)
    print('milestones: %d/%d  (%s%s)' % (len(passed), len(MILESTONES), os.path.relpath(exe, ROOT),
                                         ', original code' if args.original else ''))
    for name, _ in MILESTONES:
        print('  [%s] %s' % ('x' if name in passed else ' ', name))
    for l in last:
        print('  ' + l)
    if args.original:
        return 0

    health = lift_health()
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no terminator, '
          '%(unresolved_itail)d unresolvable ITAIL labels' % health)
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None
    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unresolved_itail'):
            if now[k] > base[k]:
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to conformance.json')
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
