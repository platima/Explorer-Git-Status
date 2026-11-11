# Explorer-Git-Status

A small Windows File Explorer extension that puts a coloured git status badge
on repository folders, in the same corner OneDrive uses for its sync status.

| Badge | Meaning |
| --- | --- |
| ![clean](docs/clean.png) Green tick | Clean: nothing to commit, in sync with upstream (or no upstream) |
| ![modified](docs/modified.png) Red dot | Modified: staged, unstaged, untracked or conflicted changes |
| ![ahead/behind](docs/aheadbehind.png) Blue arrows | Clean, but the branch is ahead of and/or behind its upstream |

Only folders that are the root of a repository (they contain a `.git` folder,
or a `.git` file for worktrees) get a badge. Ordinary folders and files are
left alone.

## Explorer never waits for git

Explorer asks for overlays on its UI thread, so the extension never runs git
there. Each lookup only reads an in-memory cache:

1. The first time a folder is shown it has no badge, and the folder is queued
   for a background worker thread.
2. The worker runs `git status --porcelain=v2 --branch` and caches the result,
   then asks Explorer to redraw that folder so the badge appears. Folders
   asked about most recently are checked first.
3. Later lookups use the cached result. Once an entry is older than 30
   seconds, the next time Explorer asks about that folder (a redraw, a
   navigation or F5) the old badge is shown and a fresh check runs in the
   background. Explorer only redraws when the badge changes.

There is no file watching. A badge can be up to 30 seconds out of date, and it
only updates when Explorer redraws the folder.

Git runs at below-normal priority with `--no-optional-locks`, so it never
takes `index.lock` and can't get in the way of your own git commands. A run
that takes longer than 15 seconds is killed. Git runs with the repository
folder as its working directory, so for the fraction of a second a check is
running Windows will not let you rename, move or delete that folder.

## Safety when browsing untrusted folders

Browsing to a folder is enough to trigger a check, and an extracted download
can contain a `.git` folder written by someone else. The extension therefore
doesn't let a repository's own configuration run programs or reach other
files:

* Repositories whose own config (`.git/config`, or the per-worktree config)
  defines any of these get no badge: `filter.*.clean` or `filter.*.process`
  commands, Git LFS `lfs.extension.*` commands, `include` or `includeIf`
  entries, or `core.excludesFile` or `core.attributesFile`. Filters set up in
  your own global or system config, such as Git LFS, still work. Repositories
  that use `include.path` in their local config therefore get no badge.
* A `.git` file that points at a network path gets no badge, and
  `core.worktree` is ignored.
* `core.fsmonitor` and hooks are turned off for the status call.
* Git is started with `GIT_ALLOW_PROTOCOL=none` and `GIT_NO_LAZY_FETCH=1`, so
  a repository can't make `git status` fetch from a remote and run its
  `core.sshCommand` or other transport commands. Git also never walks up out
  of the folder into an enclosing repository, and `GIT_DIR` and similar
  variables from Explorer's own environment are not passed on.
* Submodule working trees are not inspected (`--ignore-submodules=dirty`).
* `git.exe` is only looked up in absolute local `PATH` entries and the
  standard Git for Windows install locations. Explorer's current directory is
  never searched.

Not covered: `objects/info/alternates` in a crafted repository can still make
git touch a path of its choosing. Treat folders from sources you don't trust as
untrusted regardless of the badge.

## What is not checked

* Network (UNC) paths such as `\\server\share\repo`. Looking for `.git` on a
  share that is down can block for tens of seconds, which would hold up the
  badges for every local folder too. Mapped drive letters are checked like any
  other drive.
* Cloud placeholders (OneDrive "online only" and similar), so that nothing
  triggers a download.
* Folders inside a folder synchronised by OneDrive or a similar cloud file
  system. Windows doesn't run overlay handlers there at all (see
  [Microsoft's note](https://support.microsoft.com/help/3106961)), so a
  repository under a redirected Documents or Desktop folder normally gets no
  badge.

## Requirements

* Windows 10 or later (x64). It is built for Windows 7 and 8 as well, but that
  has not been tried.
* [Git for Windows](https://gitforwindows.org/) 2.26 or newer (for
  `git config --show-scope`). If `git.exe` is not on `PATH`, it is found in
  `%ProgramFiles%\Git\cmd` or `%LOCALAPPDATA%\Programs\Git\cmd`. If git is
  installed while Explorer is running, it is picked up within a minute.
* Git's ownership check still applies (Git for Windows 2.35.2 or newer).
  Repositories owned by another user get no badge until they are added to
  `safe.directory`. This does not protect against a repository you extracted or
  cloned yourself, which is why the other checks above exist.

## Building

Visual Studio 2019+ (Desktop development with C++) and CMake 3.15+:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
cd build
ctest -C Release --output-on-failure
```

This produces `build\Release\ExplorerGitStatus.dll`. The C runtime is linked
statically, so you don't need a redistributable.

### Tests

* `gitstatus_tests` covers the parser, the settings checks and the cache. It
  builds and runs on any platform: `cmake -S . -B build && cmake --build build
  && cd build && ctest`.
* `windows_integration` runs on Windows only. It runs the real `git.exe`
  against throwaway repositories (clean, modified, ahead and behind,
  worktrees, hostile configuration) and loads the built DLL by path to check
  the COM classes, the icons and the badge each folder ends up with. It does
  not register anything. It is skipped if `git.exe` can't be found.

GitHub Actions builds with MSVC, runs both sets of tests, checks the DLL's
exports and that it has no runtime dependency, and tries `regsvr32` and
`regsvr32 /u` on the hosted runner.

### MinGW-w64

You can also cross-compile from Linux. On Debian or Ubuntu:

```sh
sudo apt-get install g++-mingw-w64-x86-64-posix
cmake -S . -B build-mingw \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
cmake --build build-mingw
```

The posix thread model is needed for `std::thread`. This builds the DLL and
the test programs; the CI job for it only compiles.

## Installing

Copy the DLL somewhere permanent, such as
`C:\Program Files\ExplorerGitStatus\`. Then, from an **elevated** command
prompt:

```bat
regsvr32 "C:\Program Files\ExplorerGitStatus\ExplorerGitStatus.dll"
```

Restart Explorer (or sign out and back in) so it loads the new overlays.

To uninstall, also from an **elevated** prompt:

```bat
regsvr32 /u "C:\Program Files\ExplorerGitStatus\ExplorerGitStatus.dll"
```

### Upgrading and deleting the DLL

Once a program has asked for an overlay, the DLL stays loaded in it until it
exits, by design: it has a worker thread. That includes Explorer and any other
program that has shown a file dialog. So before you replace or delete the DLL,
unregister it, then restart Explorer, close other programs (or sign out, or
restart Windows), and only then replace or delete the file and register it
again.

### Not seeing the badges?

Windows has 15 icon overlay slots, reserves 4 of them, and only uses the first
11 handlers registered, sorted by name under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers`.
OneDrive alone takes several, and other products (Dropbox, TortoiseGit and
similar) compete for the rest. Some products start their names with spaces to
sort first, and this extension does the same: it registers `   GitStatus1Clean`,
`   GitStatus2Modified` and `   GitStatus3AheadBehind` (three leading spaces).
If they fall outside the first 11, rename them or remove overlays you don't
use, then restart Explorer.

## Why a badge and not a separate column?

Explorer's "Status" column belongs to the Cloud Files API and only works
inside a registered cloud sync root. Custom columns come from property
handlers, which can only be registered per file extension, not for folders.
So the extension uses icon overlays, which put the badge on the folder icon
in every view, including Details.

## Project layout

| Path | Purpose |
| --- | --- |
| `src/GitStatus.*` | Parses `git status --porcelain=v2 --branch` output and holds the safety checks on settings and paths (portable) |
| `src/StatusCache.*` | Non-blocking cache with a background worker (portable) |
| `src/GitQuery.*` | Locates and runs `git.exe` (Windows) |
| `src/OverlayHandler.*` | `IShellIconOverlayIdentifier` COM classes, one per badge colour |
| `src/Dll.cpp` | DLL entry points and registration |
| `icons/` | Overlay icons, regenerate with `python tools/make_icons.py` |
| `tests/` | Unit tests for the portable code, and Windows integration tests |
| `.github/workflows/` | CI |
