# High refresh rate (`graphics.high_refresh`)

Frames at the display's refresh rate, the simulation at its own 60 steps a
second, and the pictures between two steps blended. Overlay Graphics page,
"High refresh rate"; `settings.toml` `[graphics] high_refresh`;
`RECOMP_GFX_HIGH_REFRESH=1`. Off by default on every platform.

The row is locked until "60 fps crash cameras" (`graphics.crash_60fps`) and
"60 fps split screen" (`graphics.split_60fps`) are both on, and the game
ignores the setting until they are. Without them the title asks for its own
30 Hz phases, which this clock would run at 60.

The game code is in `src/game/hirefresh.c`: five wraps in
`src/game/recomp/recomp_manual.c`, no guest patch. The toolkit side
(xboxrecomp, `takedawn` branch) is the display's rate
(`xbox_DisplayRateSet`, from the SDL event loop) and the host flip grid
(`xbox_FlipHostRate`, `src/kernel/kernel_pacing.c`, used by
`flip_pacing_swap` in `src/kernel/nv2a_pb_exec.c`).

## Why 60 was the game's own speed

`CB3Game::Update` (`sub_000165F0`) runs the steps in `0x4D53AC` (game object
`0x4A71A0` + 0x2E20C), and the stepper `sub_001B58E0` chooses them at the
end of the frame before, with a minimum of 1 (`0x16C2D`;
[race-clock.md](race-clock.md)). A host that flips faster than 60 runs the
game faster. The step loop itself (`0x16AB6`) handles a count of 0.

The stepper cannot just be given a minimum of 0:

- Its period (timer `0x4D5338` + 0x20, in 733.33 MHz ticks) is 1/60 s in a
  stage but 1/30 s in the front end (24444444 ticks). There the minimum
  alone gives 60 steps a second. With a minimum of 0 the menus ran at
  30.00 steps a second.
- It counts no time before its first step (`0x1B58FB`: no steps taken, no
  time added). A 0 from the first frame never started the clock, and the
  boot hung at step 73.
- A paused race holds the timer (+0x34). The stepper then returns its
  minimum every frame, and the pause menu runs in the steps (the stage's
  update is skipped while a player has paused, `0x16BA1`).

## What it does

**The steps.** The title's stepper runs as shipped, but its count is not
used. At the start of `Update` the count is the number of steps a 1/60 s
accumulator on the host clock holds: 0 when it holds none, at most 4 (8 at
the half rate). The menus' mode 2 keeps the lag beyond the cap and the
other modes drop it, as the title does. The frame's time is taken in whole
flip periods, with the difference from the wall clock carried until it is
half a period. `Update` runs ahead of its flip by a varying amount (3.7 to
10.6 ms between Updates at 144 Hz), and the raw wall time moved the car
0.85 to 1.15 times its average per picture.

Active only while the game's state (`0x4D53B4`) is 5: the front end, races
and the pause menu. The other states of `Update`'s jump table (`0x16CC8`:
loads, the stage's set-up, hand-overs) step once a frame by their own code
and keep flips at 60.

**The flips.** The game asks the toolkit for a host grid at the display's
rate. The toolkit holds each flip until the grid's next edge instead of the
swap's vblank. The emulated vblank goes on at 60 Hz for everything else
that counts it (the XMV player, D3D's vblank counter). The rate is the
display mode of the screen the window is on, read twice a second by the SDL
event loop. SDL2 gives whole Hz: a 174.96 Hz panel reads 175, and the grid
then gains one picture about every 25 s, which the presenter drops.

**The pictures between steps.** Two kinds of object move:

- RenderWare frames: the camera and everything else hung on an `RwFrame`.
  RenderWare 3.7's `_rwFrameSyncDirty` (`sub_001E15E0`; dirty roots at
  `0x75935C`, LTM at frame + 0x50, child + 0x98, sibling + 0x9C) runs in
  `RwCameraBeginUpdate` (`sub_001D91B0`), before the device builds the view.
  After it, each moved frame's LTM is recorded with the step count
  (`0x4A1EB4`). Each frame that moved at the last step gets, in place, its
  last two states blended at the picture's time: one step behind the clock,
  at the lag left after the frame's steps. `RwFrameGetLTM` (`sub_001DD280`)
  does the same for frames the pipelines ask for while a picture is drawn.
  The blends are put back to the true LTMs at the start of the next
  `Update`, so the simulation never reads one.
- Vehicles: race cars and traffic are not frames. `CB3RaceCar::Draw`
  (`sub_0018DE00`) and `CB3TrafficSystem::Draw` (`sub_001A4710`) pass the
  vehicle's own world matrix (car + 0x10, laid out as an `RwMatrix`) to
  `sub_000303D0`. The blend is written there for that one call and the true
  matrix put back after it, as the title itself does when it draws a car
  with another matrix (+0x19AB, `0x18DEF2`).

The position is lerped; the axes are lerped and made orthogonal again
(Gram-Schmidt) at their lerped lengths. Nothing is blended across a move of
more than 8 units in a step or a turn of more than about 45 degrees: a
camera cut, a reset, or a frame whose address was reused.

**The HUD.** `sub_000535E0` is updated once a frame with one step's time
(`MEMF(0x4AE1FC)`; [crash60.md](crash60.md)), so at 144 Hz its animations
ran 2.4 times fast. Its time is now that value times the frame's wall time
over 1/60 s.

Not blended: what is not on a frame or a vehicle matrix. Particles, skid
marks and the deformed car body's vertices still move once a step.

## Evidence

Linux (NixOS 26.11, kernel 7.2), AMD Ryzen 7 9800X3D, NVIDIA GeForce RTX
5060 Ti (driver 610.57.04), Vulkan backend, headless, render scale 1,
2026-10-18. Without a display, `RECOMP_FLIP_PACING=host:<Hz>` stands in for
the display's rate. It is vblank pacing as before unless the title asks
for the grid. Autopilot races (`tools/headless/race_drive.sh`), the pause
menu by `MIX=1`, and `BO3_PACE_LOG` per flip.

| Run | Flips/s | Steps/s |
|---|---|---|
| Setting on, 175 Hz, race | 174 to 176 | 59.8 to 60.1 |
| Setting on, 175 Hz, pause menu | 176 | 60.0 (142 before the held-timer fix) |
| Setting on, 60 fps split screen off | 60 | 60.0 (the setting stays off) |
| Setting on, 260 Hz | 230 to 257 (this machine cannot hold it) | 60.0 |

Player 1's car as drawn, once a picture (`BO3_HIREFRESH_TRACE`), its
movement per picture against the average of the 9 around it:

| Rate | p5 | p50 | p95 |
|---|---|---|---|
| 144 Hz, wall-clock frame time | 0.85 | 1.00 | 1.15 |
| 144 Hz, flip-grid frame time | 0.97 | 1.00 | 1.00 |
| 175 Hz | 0.90 | 1.00 | 1.01 |

Of 6515 pictures with the car moving at 144 Hz, 3794 ran no simulation step,
and the car was in a new place in every one.

Frame times (`RECOMP_TRACE=pacing=all,frametime=`; `tools/perf/framestats.py`,
the last 40 s of the same autopilot race, both 60 fps switches on):

| | flips/s | frame_ms p50 / p99 | late swaps | ex_busy mean | process CPU |
|---|---|---|---|---|---|
| Setting off | 59.72 | 16.67 / 16.75 | 0.1% | 2.66 ms | 86% |
| Setting on, 175 Hz | 173.92 | 5.71 / 7.27 | 0.5% | 2.56 ms | 94% |

Frame dumps from the race at 175 Hz showed the cars, the HUD and the
takedown camera drawn as at 60. The toolkit's own tests pass (13 binaries,
the overlay's 264 settings checks among them).

## Not verified

- The setting in a window: the display's rate from SDL and the overlay row
  have not been seen on screen. An earlier build of the same blending,
  started by environment variables, was played in a window at 175 Hz; that
  is where the pause menu's speed was found.
- Variable refresh, exclusive fullscreen, a window moved between displays.
- Windows, macOS (Metal), the Steam Deck, Android: not built or run.
- Two-player split screen at a high rate: no scripted drive reaches it.
- Render scales above 1 at a high rate: the GPU's share grows with every
  picture.

Debug: `BO3_HIREFRESH_STATS=1` logs once a second what was blended;
`BO3_HIREFRESH_TRACE=<file>` writes player 1's car position per picture.
