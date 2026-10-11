# One-click setup for the Streets of SimCity static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's "Step by
# step" is the same thing by hand, command for command.
#
# It checks the tools, copies your copy of the game into game\ (from the CD or
# an ISO of it), catalogs and lifts Streets.exe to C, builds ssc.exe and leaves
# a shortcut. Each step is skipped when its output already exists (-Force redoes
# them). Everything it does goes to setup.log. Nothing is downloaded but tools.
param([switch]$Force, [string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path (Split-Path -Parent $Root) 'pcrecomp' }
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 6
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
# cmd.exe, which the build runs through, splits a path at '&'.
if ($Root -match '&') {
  Say "This folder's path has an '&' in it, which the build cannot handle:" 'Red'
  Say "  $Root" 'Red'
  Say "Move the folder somewhere else (for example C:\ssc) and run Setup.cmd from there." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Ask($q) { $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Winget-Install($cmd, $id, $size) {
  if (Get-Command $cmd -ErrorAction SilentlyContinue) { return }
  Say "  $cmd is not installed."
  if (-not (Ask "  Install it now (winget, about $size)?")) { Fail "$cmd is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install $cmd yourself." }
  Exec @('winget', 'install', '-e', '--id', $id, '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { Fail "$cmd installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}

Clear-Host
Say "Streets of SimCity - static recompilation setup" 'White'
Say "You need your own Streets of SimCity CD, or an ISO of it, and about 3 GB free."
Say "Finding the functions, the lift and the build take 20 to 60 minutes, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the pipeline needs"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"
if ((Exec ($pyargs + @('-c', 'import pefile, capstone'))) -ne 0) {
  Say "  The Python packages pefile and capstone are missing (about 15 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# The two toolkit changes this game needs (README, Getting Started).
if (-not (Select-String -Quiet -Path (Join-Path $Toolkit 'tools\assets\iso_peek.py') -Pattern 'Joliet' -ErrorAction SilentlyContinue) -or
    -not (Select-String -Quiet -Path (Join-Path $Toolkit 'tools\lift\lift32.py') -Pattern '_zc' -ErrorAction SilentlyContinue)) {
  Fail "your pcrecomp at $Toolkit is older than this game needs (Joliet ISOs, loopne). Update it: git -C `"$Toolkit`" pull"
}
Say "  pcrecomp: $Toolkit"

# Visual Studio is several GB with an installer of its own, so this says what
# to install rather than installing it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) { Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again." }
Say "  Visual Studio: $vs"
Winget-Install 'cmake' 'Kitware.CMake' '30 MB'
Winget-Install 'ninja' 'Ninja-build.Ninja' '1 MB'

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of Streets of SimCity into game\"
function Is-Disc([string]$d) { return ($d -and (Test-Path (Join-Path $d 'EXE\Streets.exe')) -and (Test-Path (Join-Path $d 'BMP'))) }
if ((Test-Path 'game\Streets.exe') -and -not $Force) {
  Say "  Already in game\ (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game) {
    foreach ($d in (Get-PSDrive -PSProvider FileSystem).Root) { if (Is-Disc $d) { $Game = $d; Say "  Found the CD in $d"; break } }
  }
  if (-not $Game) {
    $iso = Get-ChildItem -Path $Root, (Join-Path $Root 'original'), (Join-Path $env:USERPROFILE 'Downloads') -Filter '*.iso' -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match 'street' } | Select-Object -First 1
    if ($iso) { $Game = $iso.FullName; Say "  Found an ISO: $Game" }
  }
  while (-not ((Is-Disc $Game) -or ($Game -match '\.iso$' -and (Test-Path $Game)))) {
    $Game = (Read-Host "  Put the CD in, or paste the path of your ISO (or of a copy of the CD's files)").Trim('"', ' ')
    if (-not $Game) { foreach ($d in (Get-PSDrive -PSProvider FileSystem).Root) { if (Is-Disc $d) { $Game = $d; break } } }
  }
  $env:PCRECOMP = $Toolkit
  Run "Copying the game (about 450 MB)" ($pyargs + @('tools\gamefiles.py', $Game))
}

# ---------------------------------------------------------------- catalog
Step 3 "Finding the functions in Streets.exe (about 6 minutes)"
if ((Test-Path 'work\functions.json') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:PCRECOMP = $Toolkit
  Run "Cataloguing" ($pyargs + @('tools\catalog.py'))
}

# ---------------------------------------------------------------- lift
Step 4 "Lifting Streets.exe to C (about 2 minutes)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:PCRECOMP = $Toolkit
  Run "Lifting" ($pyargs + @('run_lift.py'))
}

# ---------------------------------------------------------------- build
Step 5 "Building build\ssc.exe (15 to 45 minutes)"
if ((Test-Path 'build\ssc.exe') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=`"$Toolkit`""
  Run "Compiling" @('cmd', '/c', ('call "' + (Join-Path $Root 'build.cmd') + '"'))
}

# ---------------------------------------------------------------- shortcut
Step 6 "Making the shortcut"
$lnk = Join-Path $Root 'Streets of SimCity (recomp).cmd'
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  'rem Plays the recompiled Streets of SimCity in its own window. F11 or Alt+Enter: fullscreen.',
  'cd /d "%~dp0"',
  'start "" build\ssc.exe --run')
Say "  $lnk"
Write-Host ""
Say "Done. Double-click 'Streets of SimCity (recomp).cmd' to play." 'Green'
Read-Host "Press Enter to close" | Out-Null
