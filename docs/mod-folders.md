# Mod folders

A mod folder holds files that replace files on the disc. The game reads the
folder's copy instead of the disc's; a file the folder does not have is read
from the disc as before. Nothing is written to the game data, and switching a
folder off brings the original files back.

This is for people making or trying mods of the game's own files: a track's
textures or props in its `static.dat`, a menu file, a race's data. It does not
add anything the game cannot already load: no new tracks, cars or menu entries,
and no textures larger than the ones they replace.

Every build has it: Windows, Linux and the Steam Deck, macOS and Android
(`src/game/modfs.c`: the Win32 file API on Windows, POSIX elsewhere). The rules
below are the same on all of them.

## Turning it on

Set `TAKEDAWN_MODS` to the folder before starting the game. Several folders are
separated by `;` on every system (not `:`, which is part of a path there), up
to eight. The first folder named wins when two have the same file.

Unset, there is no mod folder and nothing changes. Files are looked up when the
game opens them, so a change takes effect at the next start of the game (a
track's files at the next load of that track at the earliest). A relative path
is taken from the folder the game starts in.

**Windows** (Command Prompt):

```
set TAKEDAWN_MODS=D:\Burnout3Mods\Fixes;D:\Burnout3Mods\MyTrackMod
Takedawn.exe
```

**Linux**, from a shell:

```
TAKEDAWN_MODS="$HOME/takedawn-mods/MyTrackMod" ./burnout3
```

The installer's launcher (`burnout3.sh`, `tools/install/linux.py`) also takes
`KEY=VALUE` lines from `launch.env` in the install root, so a line
`TAKEDAWN_MODS=/home/me/takedawn-mods/MyTrackMod` there sets it for every
start.

**Steam Deck**, with the native Linux build added to Steam: the game's
Properties, Launch Options:

```
TAKEDAWN_MODS="/home/deck/takedawn-mods/MyTrackMod" %command%
```

Put the folder somewhere the `deck` user can read and write, in the home folder
or on the SD card (`/run/media/...`), not inside the game's install folder. The
home folder and an SD card SteamOS formatted are case-sensitive (ext4): that is
handled, see Layout. The Windows build under Proton is the Windows case: its
paths are Wine's (`Z:\home\deck\...`).

**macOS**: a program started from the Finder or the Dock does not get a shell's
variables, so start the game's own executable in the bundle from Terminal:

```
TAKEDAWN_MODS="$HOME/Library/Application Support/Takedawn/mods" \
    /Applications/Takedawn.app/Contents/MacOS/Takedawn
```

(or wherever `Takedawn.app` is). Keep mod folders out of the app bundle:
changing a file in it breaks the bundle's seal (`codesign --verify`). A folder
under `~/Library/Application Support/Takedawn` (the save folder) is a good
place; folders the system guards (Desktop, Documents, Downloads) may need
Terminal to be allowed to read them.

**Android**: no variable is needed. A folder named `mods` in the app's files
folder is the mod folder when it exists:

```
/sdcard/Android/data/io.github.xenophobentx.takedawn/files/
    Burnout 3 Takedown/
    mods/
        Tracks/US/C3_V1/static.dat
```

It is reached the same way as the game data (adb, or USB file transfer); the
app asks for no permission for it. A `TAKEDAWN_MODS=...` line in `env.txt`
there names other folders instead (folders the app may read: its own), and an
empty `TAKEDAWN_MODS=` turns the `mods` folder off.

## Layout

A mod folder mirrors the disc. To replace the disc's
`Tracks\US\C3_V1\static.dat`:

```
MyTrackMod/
    Tracks/US/C3_V1/static.dat
```

Names are matched without regard to case, as on the disc (the game itself asks
for some files in other cases than the disc has). On a volume that does not
fold case (Linux, the Steam Deck, a case-sensitive Mac volume) each part of the
path is taken as the game writes it when the folder has it, else the one entry
that differs only in case. If a folder has two or more entries that differ only
in case (`Data` and `DATA`) and none is spelled as the game asks, that file is
refused and the disc's is read (the log says so, once): which one the system
lists first is not a rule. Give each name one spelling.

## What is refused

The disc's own file is read instead when:

- the folder has no such file, or the name is a folder;
- something below the mod folder is a link: a symbolic link, and on Windows a
  junction or any other reparse point (a mod folder cannot point outside
  itself). The mod folder itself may be reached through links: its full path
  is resolved once, at the start;
- the file is not a regular file (a pipe, a device);
- a path part is `.` or `..`, ends in a dot or a space, or holds a drive,
  `:`, a character Windows does not allow in a name (`< > " | ? *`), a control
  character or a byte that is not plain ASCII;
- two names in the folder differ only in case (see Layout);
- the resulting path is too long for the toolkit (260 characters).

These rules are the same on every system, so a mod folder that works on one
works on the others.

A folder whose full path is not plain ASCII is ignored (on Windows the path is
read in the guest's code page; elsewhere, in the same way, a `\` in it is not
allowed either). A folder that does not exist is ignored.

The checks are made when the game opens a file; the toolkit then opens the
path. Someone who can change the mod folder while the game runs could swap a
part for a link in between: a mod folder is trusted as much as the files in it.
Nothing in a mod folder is run; the game only reads it.

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

On Linux, macOS and Android the host path is the folder's own spelling, for
example `/home/deck/takedawn-mods/MyTrackMod/Tracks/US/C3_V1/static.dat`.

The log is stderr; `RECOMP_STDIO_LOG=<file>` writes it to a file
([running.md](running.md)). On Android it is `takedawn.log` in the app's
files folder and logcat (tag `takedawn`).

## How it works

`bo3_modfs_init` (called after the soundtrack is set up) reads
`TAKEDAWN_MODS` and, if a folder was taken, chains a function onto the
toolkit's `xbox_PathRedirectHook`. For a `D:\` or `\Device\CdRom0\` path whose
file a folder has, it returns the file's host path: `\\?\<path>` on Windows,
the absolute path elsewhere. The toolkit's path translation hands either to the
host unchanged. Every other path goes on down the chain. The result buffer is
per thread. With the variable unset (and, on Android, no `mods` folder), the
hook is not installed.

On Windows each part is checked with `GetFileAttributesA`. Elsewhere the lookup
walks from the mod folder down with directory descriptors (`openat` with
`O_NOFOLLOW`, `fstatat` with `AT_SYMLINK_NOFOLLOW`) and reads a directory
(`fdopendir`) only when a part is not there as written.

`tests/modfs` checks the rules on folders it makes in the temporary directory
(no game data): `ctest -R bo3_modfs`.

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

Not run on Linux, macOS, the Steam Deck or Android yet: the POSIX code there
and `tests/modfs` on those systems are to be checked by building and running
them (the test's link, pipe, permission and case-sensitivity cases run only
there). Files other than `static.dat` in a race are not checked.
