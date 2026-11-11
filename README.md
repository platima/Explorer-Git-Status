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
   then asks Explorer to redraw that folder so the badge appears.
3. Later lookups use the cached result. After 30 seconds an entry is
   refreshed in the background, and the old badge stays visible until the
   new result arrives. Explorer only redraws when the status changes.

Git runs at below-normal priority with `--no-optional-locks`, so it never
takes `index.lock` and can't get in the way of your own git commands. A run
that takes longer than 15 seconds is killed.

## Safety when browsing untrusted folders

Browsing to a folder is enough to trigger a check, so the extension doesn't
let a repository's own config run programs:

* `core.fsmonitor` is turned off for the status call.
* Submodule working trees are not inspected (`--ignore-submodules=dirty`).
* Repositories whose own config (`.git/config`, including any files it
  includes) defines `filter.*.clean` / `filter.*.process` commands get no
  badge. Filters set up globally, such as Git LFS, still work.
* `git.exe` is only looked up in absolute `PATH` entries and the standard Git
  for Windows install locations. Explorer's current directory is never
  searched.

## Requirements

* Windows 7 or later (x64).
* [Git for Windows](https://gitforwindows.org/) 2.26 or newer (for
  `git config --show-scope`). If `git.exe` is not on `PATH`, it is found in
  `%ProgramFiles%\Git\cmd` or `%LOCALAPPDATA%\Programs\Git\cmd`.
* Git's normal ownership check still applies. Repositories that git rejects
  as "dubious ownership" get no badge until they are added to
  `safe.directory`.

## Building

Visual Studio 2019+ (Desktop development with C++) and CMake 3.15+:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

This produces `build\Release\ExplorerGitStatus.dll`. The C runtime is linked
statically, so you don't need a redistributable.

You can also cross-compile with MinGW-w64, for example
`-DCMAKE_TOOLCHAIN_FILE=<toolchain using x86_64-w64-mingw32-g++ and windres>`.
The parser and cache tests build and run on any platform:
`cmake -S . -B build && cmake --build build && ctest --test-dir build`.

## Installing

Copy the DLL somewhere permanent, such as
`C:\Program Files\ExplorerGitStatus\`. Then, from an **elevated** command
prompt:

```bat
regsvr32 "C:\Program Files\ExplorerGitStatus\ExplorerGitStatus.dll"
```

Restart Explorer (or sign out and back in) so it loads the new overlays.

To uninstall:

```bat
regsvr32 /u "C:\Program Files\ExplorerGitStatus\ExplorerGitStatus.dll"
```

Restart Explorer again before you delete the DLL.

### Not seeing the badges?

Windows only shows the first 15 icon overlay handlers, sorted by name under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers`.
OneDrive, Dropbox, TortoiseGit and others compete for these slots by starting
their names with spaces. This extension registers `   GitStatus1Clean`,
`   GitStatus2Modified` and `   GitStatus3AheadBehind` (three leading spaces).
If they fall outside the first 15, rename them or remove overlays you don't
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
| `src/GitStatus.*` | Parses `git status --porcelain=v2 --branch` output (portable) |
| `src/StatusCache.*` | Non-blocking cache with a background worker (portable) |
| `src/GitQuery.*` | Locates and runs `git.exe` (Windows) |
| `src/OverlayHandler.*` | `IShellIconOverlayIdentifier` COM classes, one per badge colour |
| `src/Dll.cpp` | DLL entry points and registration |
| `icons/` | Overlay icons, regenerate with `python tools/make_icons.py` |
| `tests/` | Unit tests for the parser and cache |
