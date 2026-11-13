# Minute drill - design

Date: 2026-11-13. Play script (`examples/step1_script.txt`) plus three new script tools in the next mod
build. Replaces the Stop and go button and section.

## Why

Stop and go was fine in theory but not in play. The player wants a tempo mode instead: playing fast
throws the CPU defense off its rhythm, and the mode rewards that, the way real hurry-up offense does.
It is balanced by tempo itself (slow snaps earn nothing) but must not be nerfed into irrelevance.

## Goals

- A **Minute drill** pre-snap button. Pressed once, the mode stays on until the current drive ends
  (score, turnover, punt, turnover on downs, end of half).
- Each play in the mode is **on tempo** when it is snapped within `md_window` seconds (default 10) of
  the previous play ending. The snap that turns the mode on is always on tempo.
- On-tempo plays get all three rewards:
  1. **Quick short completions gain guaranteed yards.** A catch thrown quickly and short leaves the
     nearest defenders a step slow, so the catch reliably gains at least a few yards.
  2. **Juke prompt.** On a completion over 5 yards, when a tackler closes on the catcher, a JUKE button
     flashes on the screen edge. A second-finger tap on it in time makes that tackle miss (the tackler
     is pushed off and slowed for a moment) and the carrier gains extra yards.
  3. **Receivers block harder on runs.** On a handoff, receivers go find the nearest defender and hold
     him: steered into him, and he is slowed while they are engaged.
- A slow snap keeps the mode on but earns nothing; a message says so before the snap.

## Non-goals

- Changing the game clock or the CPU's play calling.
- Jukes outside the mode, or on the QB scrambling.
- Any change to the coordinator brain (it keeps calling routes in the mode).

## Anti-goals

- So strict it's never worth turning on (hence the forgiving 10 s window and first-snap rule).
- A juke that fires by itself: it needs the tap.
- A prompt that steals the touches the game uses to run and dive (hence the second finger, as the
  pump fake already does).

## Approaches considered

- **A. All in C.** Solid timer and drive detection, but every behaviour change needs a build and the
  script tools go unused.
- **B. Script only.** Ruled out: scripts can't measure real time between plays, read a tap during a
  run, or draw the prompt.
- **C. Script plus three new script tools in the mod build (chosen).** C adds only what scripts can't
  do; the script owns the mode, the rules and the numbers. Later tuning is script-only, and the tools
  can serve future features.

## The three new script tools (mod build)

- **`tempo()`**: seconds of real time (frames / 60) from the end of the previous play to this snap.
  Valid in `on snap` and later in the play; -1 if there was no previous play this game.
- **`on drive_end`**: an event that runs when the player's team loses the ball (the existing
  `change_possession` hook in `src/hooks.c`, when possession leaves the human team) and when a half
  ends.
- **`juke_ready(player)`**: arms the juke for that player for the rest of the play. While armed, each
  frame the mod checks for a defender within `juke_reach` (Tune tab, default 2.5 yards) and closing.
  Then it shows a JUKE edge button (the same panel kind as the pump fake's PUMP, `rb_ui_panel`) for
  `juke_window` (default 0.6 s). A tap in that window fires the script event **`on juke`**; inside it,
  **`tackler()`** is the defender who was closing. No tap: the button hides, and the play goes on
  normally. One juke per play.

## The script section (replaces 3. STOP-AND-GO)

Tunables at the top, with plain-English comments:

| Name | Default | Meaning |
|------|---------|---------|
| `md_on` | 1 | 1 = the Minute drill button shows |
| `md_window` | 10 | seconds after a play ends you have to snap and stay on tempo |
| `md_quick` | 1.5 | a throw out within this many seconds of the snap counts as quick |
| `md_short` | 5 | a catch within this many yards past the line counts as short |
| `md_slow_pct` | 60 | how fast the nearest defenders move for the rest of a quick short catch, percent |
| `md_slow_n` | 2 | how many defenders are slowed on a quick short catch |
| `md_juke_yards` | 5 | completions longer than this arm the juke |
| `md_juke_push` | 3 | yards the juked tackler is pushed off |
| `md_block_pct` | 70 | a run-blocked defender's speed, percent |

Flow:
- `on presnap`: the button; messages: `Minute drill: on tempo` / `Minute drill: too slow - defense set`
  (shown for the play being set up, judged at the snap; the pre-snap message uses the time so far).
- `on snap`: if the button is pressed, the mode turns on (first snap = on tempo). If the mode is on,
  `md_tempo = tempo() >= 0 and tempo() <= md_window` (first snap: 1).
- `on catch` (Extreme only, like all catch events): if on tempo, the catch spot's depth from the line
  decides: quick (`since_snap()` at the throw <= `md_quick`) and short (<= `md_short` yards) → slow the
  `md_slow_n` defenders nearest the catcher (`speed()`, once). Over `md_juke_yards` → `juke_ready` on
  the catcher (nearest offense player to the ball).
- `on juke`: push `tackler()` `md_juke_push` yards away from the carrier (steered for a moment, as the
  super throw clears defenders) and slow him for the rest of the play.
- `on frame`: if on tempo and the play is a run (the ball handed off: `state()` reads the handoff state,
  or the back has the ball and nothing was thrown), each receiver is steered into his nearest defender,
  and a defender in contact is slowed to `md_block_pct` once.
- `on drive_end`: the mode turns off, with a log line.

## Testing

- Here: the script loads under the Workshop checker within the limits (48 blocks, 64 variables, alone
  and with the Hail Mary). The C tools get log lines; `tempo()`'s frame counting and the juke's
  distance-and-closing check go in a pure C file with a CI test (as `src/bullet.c`).
- On the phone, one Extreme game with the mode on for a few drives: the log shows tempo per snap, the
  rewards that fired, juke prompts shown / tapped / missed, and the mode ending with each drive.

## Risks

- How the game decides a tackle is unknown; the juke works around it (push and slow the tackler before
  contact) rather than cancelling a tackle. If contact happens before the prompt, the reach or window
  needs tuning.
- `on catch` is Extreme only; the mode's catch rewards don't work on other difficulties.
- At most 3 pre-snap buttons: with Super throw, Screen and Minute drill, the Hail Mary at the end of a
  half pushes one off.

## Settled after the builder check

- **Quick is measured at the release:** the script stores `since_snap()` in `on throw` (an existing
  event) and checks it in `on catch`. No new tool.
- **Short is measured from the catch spot:** in `on catch`, the ball's x against `los_x()`. Extreme only,
  as the risks say.
- **The juke tap during a run is unproven.** The mod reads second-finger taps while aiming; reading them
  while the receiver runs hasn't been tried. The build logs every touch it sees while a juke is armed,
  so the first test game shows whether taps arrive. If they don't, the next build changes the input.
  JUKE is a panel button (pump fake pattern), not a tap anywhere.
- **End of the previous play (for `tempo()`):** the frame the ball goes dead (tackle, incompletion,
  score, sack, interception: the ball states `offtools.c` already watches), on any difficulty. The
  time counts the result pause before the next play; the 10 s default allows for it.
- **Drive end:** `change_possession` away from the player covers scores (the kickoff), punts, turnovers,
  turnovers on downs, safeties and missed field goals. A half ending is detected by the quarter
  changing from 2 to 3 (or the game ending) at the next pre-snap.
- **Messages:** while the mode is on, every pre-snap shows `Minute drill: snap fast!`, prefixed with
  `Last snap too slow.` when the previous play missed the window. No live countdown.
- **Button limit:** once the mode is on it no longer needs its button, so if the Hail Mary pushes it off
  at the end of a half, the mode carries on. Turning the mode on at the very end of a half may not be
  possible then.
- **Run detection:** the ball's state 19 is a handoff (`dbrain.c` already uses it), so `state() == 19`
  marks a run.

## Builder's calls (not discussed with the player)

- Second-finger tap on the JUKE button (pump fake pattern), and its edge position.
- How "closing" is measured, frame counting for tempo, and what counts as the end of a play.
- The reward sizes above (all script tunables).
- Run detection in the script.
