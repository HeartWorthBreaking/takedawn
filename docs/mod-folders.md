# Mod folders

A mod folder holds files that replace files on the disc. The game reads the
folder's copy instead of the disc's; a file the folder does not have is read
from the disc as before. Nothing is written to the game data, and switching a
folder off brings the original files back.

This is for people making or trying mods of the game's own files: a track's
textures or props in its `static.dat`, a menu file, a race's data. It does not
add anything the game cannot already load: no new tracks, cars or menu entries,
and no textures larger than the ones they replace.

Windows only for now (`src/game/modfs.c` uses the Win32 file API; the other
builds do not compile it).

## Turning it on

Set `TAKEDAWN_MODS` to the folder before starting the game:

```
set TAKEDAWN_MODS=D:\Burnout3Mods\MyTrackMod
Takedawn.exe
```

Several folders are separated by `;`, up to eight. The first folder named wins
when two have the same file:

```
set TAKEDAWN_MODS=D:\Burnout3Mods\Fixes;D:\Burnout3Mods\MyTrackMod
```

Unset, there is no mod folder and nothing changes. Files are looked up when the
game opens them, so a change takes effect at the next start of the game (a
track's files at the next load of that track at the earliest).

## Layout

A mod folder mirrors the disc. To replace the disc's
`Tracks\US\C3_V1\static.dat`:

```
MyTrackMod\
    Tracks\US\C3_V1\static.dat
```

Names are matched without regard to case, as on the disc.

## What is refused

The disc's own file is read instead when:

- the folder has no such file, or the name is a folder;
- something below the mod folder is a symbolic link or a junction (a mod
  folder cannot point outside itself);
- a path part is `.` or `..`, ends in a dot or a space, or holds a drive,
  `:` or a character Windows does not allow in a name.

A folder whose full path is not plain ASCII is ignored (the path is read in the
guest's code page). A folder that does not exist is ignored.

## What is not checked

The game's own loaders read the file. Nothing checks that a replacement is a
valid file of its kind: a damaged or mismatched file can make the game draw
garbage, show the missing-texture colour (magenta), stall or crash. Replace a
file only with one made for the same file of the same disc (the NTSC-U disc
Takedawn installs, manifest `81dc20fd647fb0b1`), and keep its size unless you
know the format allows otherwise.

## Seeing what happened

Every folder taken and every replaced file is written to the log, once:

```
[MOD] folder 1: D:\Burnout3Mods\MyTrackMod
[MOD] D:\tracks\US\C3_V1\static.dat -> D:\Burnout3Mods\MyTrackMod\tracks\US\C3_V1\static.dat
[MOD] ignored (not a folder): D:\Burnout3Mods\Typo
```

The log is stderr; `RECOMP_STDIO_LOG=<file>` writes it to a file
([running.md](running.md)).

## How it works

`bo3_modfs_init` (called after the soundtrack is set up) reads
`TAKEDAWN_MODS` and, if a folder was taken, chains a function onto the
toolkit's `xbox_PathRedirectHook`. For a `D:\` or `\Device\CdRom0\` path whose
file a folder has, it returns `\\?\<the file's host path>`, which the toolkit's
path translation hands to the host unchanged. Every other path goes on down the
chain. The result buffer is per thread. With the variable unset, the hook is
not installed.

## Verified

Windows 11 (build 26300), clang 23.1.2 (llvm-mingw 20260922), Vulkan on an
AMD Radeon RX 9070 XT, the NTSC-U disc; 2026-10-21. Silver Lake (US/C3_V1)
Single Event race, from the main menu: with the variable unset, the game is
unchanged and the retail texture is uploaded; with a folder holding a
`static.dat` whose junction texture was recoloured, the recoloured texture is
drawn and the original is not uploaded. Both finish a full three-lap race with
the game's AI driver. A folder that does not exist, an empty folder and a file
reached through a junction leave the retail file in use; of two folders with
the same file the first one's is read; a malformed texture record is drawn in
the missing-texture colour without a crash. The path refusals, a drive root as
a folder and eight threads looking files up at once were checked with
synthetic files. No measurable change in frame times with the variable set.
Not checked: Linux, macOS, the Steam Deck and Android (not built there);
files other than `static.dat` in a race.