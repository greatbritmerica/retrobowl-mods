# Step 1: four play scripts (no new IPA)

Date: 2026-11-12. Needs mod build v89 or later (script engine, `docs/SCRIPTING.md`).

## Goal

The user wants to add many features without a prompt to Claude for every small change. Step 1
delivers four features as play scripts. The user installs them by pasting into the Workshop's
Script tab and saving, and tunes them by editing plain-English numbers at the top of each section.
No IPA, no Workshop changes.

## Packaging

- One file, `examples/step1_script.txt`, because the engine reads only `rbmod_script.txt`. The user
  pastes it into the Script tab, alongside the Hail Mary script if they use it, and saves.
- Four labeled sections. Each section's first `on presnap` block sets its tuning numbers, each with a
  `#` comment saying what it does, plus an on/off switch (`super_on = 1`; 0 turns the feature off).
- Buttons are added by the script with `button("…")` in `on presnap`. No rules file is needed.
- Every feature writes `log(...)` lines prefixed `s1:` so one test game shows what each did.
- Before delivery, the file passes the Workshop's own checker (`site/script.js`), run here under Node.

## 1. Super bullet throw

- **Meter:** a number variable (it persists between plays). `on catch` adds `meter_per_catch` (default 20).
  `on incomplete` and `on intercepted` subtract `meter_per_miss` (default 10). Kept between 0 and 100.
- **Display:** in `on presnap`, `msg("Super meter N%")`.
- **When full** (`meter >= super_needs`, default 100): a **Super throw** button.
- **When pressed and the ball is in the air** (`on frame`, `thrown()`): the receiver nearest
  `landing_x(), landing_y()` steers there at full strength, the same mechanism the Hail Mary uses on
  device. `on snap` sets `tune("pick_base", 0)` and `tune("pick_known", 0)` for that play. If
  `super_slow_defenders = 1`, the defenders nearest the landing spot get `speed(d, super_def_speed)`
  (default 70%).
- **After the throw:** the meter resets to 0 at `on play_end` when the button was used.
- **Not in step 1:** aiming by tapping a receiver (needs tap input, step 2).

## 2. Screen pass

- **Button:** Screen, every play while `screen_on = 1`.
- **At the snap:** `route(back(), "route_flat")`. The `screen_linemen` (default 3) offensive players
  nearest the line of scrimmage on the screen side, excluding the QB, the back and the receivers,
  are found with `nearest()` and steered toward a spot `screen_depth` yards (default 3) outside the
  back, in `on frame`, until the ball is caught.
- **Tuning:** `screen_side` (1 = top, -1 = bottom, 0 = the side with more room),
  `screen_depth`, `screen_linemen`.

## 3. Trick plays

- **Stop-and-go:** a button that gives every receiver `route_slant_fake` at the snap. The route
  already exists in the game. Tuning: on/off only.
- **Fake handoff:** not added. The existing Play Action button already does this.
- **Hook-and-lateral and flea flicker:** blocked on one unknown, how the ball records who holds it.
  Step 1 ships a probe instead: in `on frame` it logs `ball().holder` and `ball().state` whenever
  they change (`s1: holder …`). After one test game:
  - if `holder` is a player value a script can set, both plays are written as scripts (no IPA);
  - if not, they move to the step 2 IPA.
  The probe is switched off with `probe_on = 0`.

## 4. Light injuries

- **When:** at `on play_end` after a run or a catch that ends in a tackle, or `on sack`, the player
  nearest the ball (`nearest(offense(), ball().x, ball().y, 1)`) is hurt with `injury_chance`
  percent chance (default 3). Sacks count when `injury_sacks = 1`.
- **Effect:** `speed(player, injury_speed)` (default 85%) on every later play of that game, and a
  pre-snap `msg` naming the injury once.
- **Remembering the player:** players are dropped between plays, so the script stores a number
  that identifies him. The probe logs candidate identity values on the injured player
  (`s1: injured id …`). The first test game confirms which one is stable across plays. Until then,
  the effect applies only on the play he was hurt.
- **New game:** detected when `quarter()` goes down. Injuries clear.

## Testing

- Here: every section loads under the Workshop checker. Each block's logic is checked by reading,
  since the game can't run here.
- On device: one test game, then the user sends `rbmod.log`. Expected lines: `script: self-test passed`,
  `script: rbmod_script.txt loaded - N on blocks`, and `s1:` lines for each feature used.
- After that, tuning is the user's job via the numbers at the top. Real bugs go back to Claude, batched.

## Risks

- Super throw: steering the receiver works for the Hail Mary, but a short throw gives him less time
  to get there. If it misses, `super_def_speed` and the steer strength are the first knobs.
- Screen pass: picking linemen with `nearest()` may grab a receiver lined up tight. The log names who
  was picked.
- Injuries and the trick-play probe depend on game values not yet confirmed. Each has a fallback,
  so neither blocks the other features.

## Next (designed after the step 1 test game, not now)

- Step 2, one IPA: coin toss with a heads/tails call, tap-to-target super throw, substitutions,
  injuries that take a player out, wildcat. It uses what the step 1 probe finds about the ball holder
  and player identity.
- Step 3, one at a time: punt return (built from kickoff return), press conferences (on the game's
  dilemma system) and weekly practice.
