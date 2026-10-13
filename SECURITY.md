# Security

## What the program touches

- **A drive letter.** `ssc.exe` maps a free drive letter onto the game folder
  for the current logon session (`DefineDosDevice`, what `subst` does; no
  administrator rights) so the game finds its CD. A clean exit removes it; a
  crashed run's letter is reused by the next run and goes at logoff.
- **Files.** The game reads its data and writes careers and saves in its own
  folder (`game\Games\`). With Player's Choice it opens the SimCity 2000 city
  you pick in a File dialog, or the one `--open` names.
- **Processes.** `--record` starts `ffmpeg` from `PATH`.
- **Setup.** `Setup.cmd` installs missing prerequisites with `winget` and
  `pip`, and clones pcrecomp with `git`, each only after asking.

In the paths exercised so far it makes no network connections and handles no
credentials. The game's own network play (DirectPlay, Winsock) is in the code
but has not been brought up.

## How it runs code

The game's code is compiled C running in-process, and calls the real Win32 API
with the game's arguments. A city or save file is parsed by the game's own 1997
code, so it should be treated with the caution due any old program opening
untrusted files.

## Reporting

Report a problem privately through GitHub's *Report a vulnerability* on this
repository, rather than in a public issue.
