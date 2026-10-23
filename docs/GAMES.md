# Games

Four programs live in `user/bin/`: `danmaku`, `minesweeper`, `snake` (all in the start menu's Games section) and the shared
art/game toolkit they use (`user/lib/kappa/include/kappa_gfx2d.h`, `kappa_gamekit.h`). All art is drawn at start-up from circles,
polygons and gradients; there are no image files and nothing is taken from the original games. There is no sound.

Every game accepts `--selftest` (runs its logic tests without a window and prints `PASS (n checks)`) and `--trace` (prints what happens,
which is what `tests/case_games.py` waits for). Scores are plain text files in `~/.config/`.

## Danmaku (`danmaku`)

A vertically scrolling bullet-hell shooter in a 640x480 window with the classic 384x448 field and a side HUD. Three stages
(Starlit Cloud Road, Whispering Bamboo Hollow, Lake of a Thousand Lanterns), each with scripted waves of fairies, then a boss; stages 2 and 3
also have a mid-boss. Five original bosses with 14 phases, 11 of them named spell cards with a timer and a capture bonus.

| Key | Action |
|-----|--------|
| Arrow keys | move |
| Z | shoot (hold) |
| X | bomb (also inside the short window after being hit: a "deathbomb") |
| Shift | focus: slow movement, shows the hitbox, tighter shots |
| Esc | pause (Resume / Restart / Quit to title); losing the keyboard focus pauses too |
| F3 | frame rate, frame time and bullet count |

* **Menus**: Start, Practice (choose a stage), High Scores, Quit. Difficulty Easy / Normal / Hard / Lunatic (changes bullet counts, speeds, which
  patterns appear), shot type A (Shrine Maiden: fanning amulets plus homing needles, bomb "Hundred Colour Orbs") or B (Witch: stars that gather into
  a beam when focused, bomb "Starlight Lance").
* **Rules**: lives and bombs, power 0.00-4.00 (P items), point items (worth more the higher you collect them; fly above the dotted line near the top
  to pull in every item), graze (500 points per bullet that passes close), extends at 5, 12, 25, 45 and 80 million. Capturing a spell card (no
  bomb, no death) pays the bonus plus a share for the time left. High scores per difficulty in `~/.config/danmaku.scores` (name entry after a
  good game over or the ending). Practice runs are not recorded.
* **Bullets**: pattern library in `bullets.c` (rings, fans, aimed shots, flowers, dense beams, accelerating, curving, gravity, stop-and-go, splitting,
  bouncing). The pool holds 1600; the shipped patterns peak below 700 on Lunatic.
* **Performance**: fixed 60 Hz logic from CLOCK_MONOTONIC with catch-up (frames are skipped, never the game speed, up to 5 steps per frame);
  integer fixed-point simulation; sprites are pre-rendered and colour-keyed, backgrounds are one `rep movsq` row copy per scanline plus a few sprites;
  only the field is committed to the window server on most frames (the HUD at most every 6th tick).
* **Testing options**: `--demo` (autopilot), `--start`, `--stage N`, `--diff 0-3`, `--shot 0|1`, `--boss`, `--midboss`, `--phase K`, `--power N`,
  `--seed N`, `--warp N` (simulate N ticks first) with `--hold` (then freeze: a still picture), `--god`, `--quit-after N`, `--noscores`, `--trace`.

## Minesweeper (`minesweeper`)

Fairies hide under the grass instead of mines; flags are ofuda talismans. Beginner 9x9/10, Intermediate 16x16/40, Expert 30x16/99 and Custom
(Game menu). The window is sized to the board. The first click is always safe (and opens an area).

| Input | Action |
|-------|--------|
| Left click | uncover (on a satisfied number: chord) |
| Right click | place / lift an ofuda |
| Middle click, or left + right | chord: uncover the neighbours of a number whose ofuda are all placed |
| Arrows, Space, F, Enter | keyboard play: move the cursor, uncover, flag, chord |
| F2 | new game; the smiley button does the same |

Best times (top 5 per level) are kept in `~/.config/minesweeper.scores`; a new record asks for a name. Options: `--level`, `--size WxH --mines N`,
`--seed N`, `--cell PX`, `--trace`.

## Snake: Eat the dango (`snake`)

A smoothly moving snake in a lantern garden. Modes: Walls or Wrap-around, start level 1-9. Every 5 dango raise the level (the snake gets
faster); stone lanterns appear from level 3, 5, 7 and 9; a golden dango turns up now and then for a bonus.

| Key | Action |
|-----|--------|
| Arrows or WASD | steer (two turns can be queued) |
| Space, P, Esc | pause (Q goes back to the title) |
| F3 | frame rate |

High scores (top 10 per mode) in `~/.config/snake.scores`. Options: `--start`, `--wrap`, `--level N`, `--seed N`, `--bot [--bot-limit N]`
(autopilot), `--trace`.

## Tests

`tests/case_games.py` runs the three self-tests in the guest and plays each game on the desktop (mouse and keyboard, pixel checks, trace lines,
score file). `tests/games_session.py script.txt prefix` is a screenshot driver like `gui_session.py` with `@expect SECONDS REGEX` (wait for a line of
output instead of sleeping) and `@hold`/`@press`/`@release` for held keys.

Note: when the host's timer resolution is coarse, the emulated guest clock can run many times slower than real time (check `cat /proc/uptime`
twice); games then run slowly in real time but correctly in guest time, which is why the tests wait for output rather than for seconds.
