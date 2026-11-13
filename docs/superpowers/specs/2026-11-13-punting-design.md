# Playable punts — design

Status: approved by the user 2026-11-13, nothing built. Needs C (`src/`) and a new IPA (see "What building it takes").

## Intent

Like defense before the mod, the game has no punting you play. When you punt, the CPU gets the ball at a
spot in the same frame. The user wants a real punt play: the snap goes to their punter, who either kicks it
with a field-goal-style swipe (aim and power) or fakes it by running or throwing.

## Goals

- On the user's 4th down, choosing to punt starts a **live play** with the punter about 15 yards deep.
- During that play the user decides: **kick** (switch to kick mode, swipe to aim and set power) or **fake**
  (a normal play, run or throw).
- The kick shows a **short ball flight** (about 2 s) from the punter to where it lands, then the CPU gets
  the ball there.
- A bad swipe (weak or wide) gives a bad punt: short, or out of bounds.

## Non-goals (for now)

- Blocked punts, muffs, a live return the user covers (option 2 below; it may come later).
- The CPU's punts. Those belong to the punt-returns work (`2026-11-13-punt-returns-design.md`).
- Changing field goals or kickoffs.

## Anti-goals (a failure even if it "works")

- Any change to how the CPU punts, or to the pass/pump-fake controls on other plays.
- A punt that leaves the game stuck (wrong possession, wrong down, a play that never ends).
- A crash on a punt. A fallback to the game's normal punt is fine; a crash is not.

## Constraints and facts

- **The game's punt is instant** (found by the punt-returns session): the punt result calls `s_punt`, then
  `s_change_possession` sets the CPU's new spot in the same frame. There's no flight and no return.
  `btn_response_punt_yes` (0x94060) is the button that starts it. `cpuqb` presses it for CPU punts.
- **The field goal kick is a hold-and-release mini-game** on the ball's values `kickpow`, `direc` and
  `kickflip` (see `PH_FG` in `src/cpuqb.c` and the kick recorder in `src/hooks.c`). It only runs from the
  field goal setup, not in the middle of a play.
- **Dragging during a live play means throwing.** The pump fake already reads a second finger while
  aiming (`src/offtools.c`). That's the switch into kick mode.
- **Only `qb()` reliably finds the same player again**, so the punter is whoever takes the snap: the QB
  (or the kicker, if the builder finds a safe way to swap him in).
- The punt-returns session also plans to hook `s_punt`. **There must be one hook**, branching on who is
  punting (the user's team: this design; the CPU: punt returns).
- Play scripts can add pre-snap buttons and `place()` players, but they can't change the kick, so this is
  C work. The user wants as few rebuilds as possible: aim for one logging build, then one feature build.

## Approaches considered

1. **The user's swipe, then the game's punt (picked).** The mod runs the kick meter mid-play. On release
   the play ends and the mod runs the game's own punt, with the distance and spot from the swipe in place
   of the game's. The game handles possession and downs, the parts most likely to break. The mod draws a
   short flight first. Trade-off: no blocks, no live return.
2. **A fully live kick.** The real ball leaves the punter's foot and the CPU fields it. The mod flips
   possession itself. Most real, most work, most risk of a broken game state. Maybe later.
3. **Decide before the snap** ("Punt" or "Fake" buttons). Easiest, but it isn't playing as the punter.
   Dropped.

The user picked 1, with **a swipe like a field goal** (not a tap-to-kick) and **a short flight** (not a
straight cut to the CPU's ball).

## Design

### Flow

1. **4th down, the user taps Punt** (the game's own 4th-down question). The mod catches the user's punt
   answer (only when the user's team has the ball) and starts a normal 4th-down play instead of the instant
   punt. The CPU's punts pass straight through.
2. **Lining up:** the punter (the QB) is placed `PUNTER_DEPTH` yards behind the line at the snap. The
   rest of the formation stays the game's (builder's call: a play script with `place()` if that's enough).
3. **Live play:** the user can:
   - **Fake:** do nothing special. It's a normal play. Gain the first down or it's a turnover on downs, as
     usual.
   - **Kick:** while holding the ball behind the line, tap a second finger. Kick mode starts: passing is
     off for the rest of the play, the pump fake is off on this play, and the kick meter shows.
4. **The swipe** feels like a field goal (my call: copy its controls exactly so it's familiar): hold to
   fill power, drag sideways to aim, release to kick. Power sets the distance, up to the punter's max.
   Aim sets the sideways angle. Too wide goes out of bounds, where the CPU gets the ball at the spot it
   went out.
5. **If the rush reaches the punter before the release**, it's a sack, as on any play. (No blocked punts.)
6. **Release:** the play stops (players freeze, the game stays in the play) and the mod draws the ball in
   flight from the punter to the landing spot, about `FLIGHT_SECONDS`, with an arc. Then it runs the game's
   punt with the landing spot, so the CPU gets the ball there (touchback rules: past the goal line = their
   20).
7. **Fallback:** if anything goes wrong (no ball, odd state, a timeout during kick mode), the mod runs the
   game's normal instant punt and logs why.

### Landing spot

- Distance = `PUNT_MIN + power × (PUNT_MAX − PUNT_MIN)`, scaled by the kicker's power rating
  (`my_kick_power`) if it's readable. Builder's call on the exact rating curve.
- Sideways = aim angle × distance. Past the sideline counts as out of bounds at the crossing point.
- `SHANK_CHANCE` adds a small random short kick to a sloppy swipe (power below `SHANK_POWER`). Builder's
  call; can be 0.

### Tunables (top of the C section, plain-English comments, Workshop-editable like the others)

| Name | Start | Meaning |
|---|---|---|
| `PUNT_ON` | 1 | 1 = playable punts, 0 = the game's instant punt |
| `PUNTER_DEPTH` | 15 | yards the punter lines up behind the line |
| `PUNT_MIN` | 20 | yards for the weakest swipe |
| `PUNT_MAX` | 55 | yards for a full-power swipe by a top punter |
| `PUNT_AIM_MAX` | 20 | widest aim, in degrees left or right |
| `FLIGHT_SECONDS` | 2.0 | how long the drawn flight lasts |
| `SHANK_POWER` | 0.25 | swipes weaker than this may shank |
| `SHANK_CHANCE` | 0.3 | chance a weak swipe shanks |

### Logging (for `/playtest`)

One line each: punt play started, kick mode on (frame, punter position), each swipe sample (power, aim),
release, landing spot, the game's punt call and the new CPU spot. On a fake: "punt fake: run/pass". On a
fallback: the reason.

## Unknowns the first (logging) build must answer

1. **What `s_punt` reads** to choose the spot (`kick_dist`, `kickx`/`kicky`, a rating, randomness), and
   whether the mod can set the spot before calling it, or must call `s_change_possession` itself with the
   spot.
2. **Whether the punt can be run mid-play** (from `on frame` / the ball update) without leaving the play
   half-open. If not, end the play first (the way a sack or an out-of-bounds does), then run the punt.
3. **How the user's Punt answer is caught**: hooking `btn_response_punt_yes` vs. the `s_punt` hook shared
   with punt returns. It must leave `cpuqb`'s own press of that button alone.
4. **Whether the game's kick meter drawing** (`s_draw_kicking_power`, `s_draw_kick_cone`) can be called on
   our play, or whether the mod draws its own meter (the mod already draws buttons and the aim line).
5. **Drawing the flight:** move the real ball sprite along the arc while the play is frozen, or draw it.

Disassembling `s_punt`, `btn_response_punt_yes` and `s_draw_kicking_power` may answer 1, 3 and 4 with no
build at all. Do that first.

## Left to the builder

- Rating curve, shank rules, how the meter looks (match the field goal's if callable).
- Whether the formation is a play script or part of the C (C is simpler if it's already in there).
- How the frozen play is held during the flight.

## What building it takes

1. Read the disassembly of `s_punt`, `btn_response_punt_yes`, `s_draw_kicking_power`, `s_draw_kick_cone`
   and `s_change_possession` (no build).
2. If unknowns remain: **one logging build** (new IPA), then the user plays one game with a 4th-down punt
   and sends `rbmod.log`.
3. Agree on the single `s_punt` hook with the punt-returns work (shared line in `src/hooks.def`).
4. **Feature build:** new `src/punt.c` (play start, kick mode, meter, landing spot, flight, the punt
   call, fallback), a hook on the user's punt answer, kick-mode check in `offtools.c` (pump fake off),
   tunables in `tools/tunables.json`, a `SCRIPTING.md` note, the Workshop's tunables page rebuilt.
5. `mod-pitfall-reviewer`, push, the CI build, the user's IPA swap, a test game, `/playtest`.

Expect one or two IPA swaps in total.
