# Offensive coordinator brain - design

Date: 2026-11-12. Mod build (C), next build after v91. Extreme only (it learns from the defense code's
per-throw results, which only run on Extreme).

## Why

The CPU defense brain (`src/dbrain.c`) adapts to how the player attacks, and that made games fresher.
The player wants the same on their own side: an offensive coordinator that stops calling plays in the
vanilla order, learns what works, and thinks outside the box. The goal is replayability on offense.

## Goals

- Before every player snap, the coordinator rewrites the receivers' routes. The player runs what it
  calls, like vanilla.
- It learns from every pass: which route gained yards, on which down and distance.
- It counters the defense brain: it calls routes into the areas the CPU defense knows least.
- It remembers across games, in a file, like the CPU brains do.
- A short line before the snap says what it called and why, so the player can see it thinking.

## Non-goals

- Formations and the run/pass call (approach C, later, after research into how the game picks them).
- Plays the player picks from (the player chose: the coordinator calls the play).
- Learning per receiver: no game value tells players apart (`CLAUDE.md`), so learning is per route and
  per position (top, middle, bottom receiver), never per player.

## Anti-goals (a failure even if it works)

- The same call every time once it finds one that works. That kills replayability.
- Calls that are clearly dumb in the situation (a 5-yard hitch on 3rd and 15).
- Taking away the player's control: hot routes after its call must still work.
- Slowing the game (one decision per play, at the snap setup, nothing per frame).

## Approaches considered

- **A. Route brain in the mod build (chosen).** Rewrites routes through the game's own route setter
  (`s_set_my_route`, already used by hot routes in `src/offtools.c`), learns from the per-throw result
  the defense code already computes, and saves to a file. Proven parts; one build.
- **B. Play script only.** No build, but scripts can't save a file between games, have a 64-variable
  limit, and `route()` errors when a route doesn't change. Forgetting between games works against
  the why.
- **C. Full playbook brain.** A plus formation and run/pass. Closest to a real coordinator, but how the
  game picks those is unknown; it starts with research. Planned as an add-on to A.

## How it works

### When

At the point where `rb_off_step` finds this play's route runners (`offtools: your snap - N route
runners`, before the player snaps). One decision per play, only when `O.offense_snap` (a real
scrimmage play) and the game is on Extreme. Not on the CPU's drives, kickoffs or two-point plays
unless they are normal scrimmage snaps.

### What it calls

- The route list is the hot route list in `offtools.c` (`ROUTES[]`: hitch, slant, dig, curl, post,
  corner), plus **keep** (leave the vanilla route the game gave him).
- Every route runner except the back gets a call. The back keeps his vanilla route (the screen
  script and play action use him).
- No two receivers get the same route when it can be avoided (spread the field).
- The vanilla route is read first (`SLOT_MY_ROUTE`, a path number) and logged, so later work can map
  path numbers to names.

### What it learns (the brain file, `Documents/rbmod_ocbrain.txt`)

For each route (7 including keep) x position (top / middle / bottom receiver) x situation (short:
1-3 yards to go, medium: 4-7, long: 8+): passes thrown to it, passes that worked, yards gained.
- "Worked" is the defense code's own test (`dbrain.c`: a touchdown, or a gain of 40% / 60% / 100% of
  the yards to go on 1st / 2nd / 3rd-4th down).
- Credit goes only to the route of the receiver the ball was thrown to (`P.rcv` in `dbrain.c`, the
  offense player nearest the landing spot). Routes nobody threw to learn nothing.
- Runs teach nothing (routes don't matter on a run).
- At the start of each new game, every count is multiplied by a decay (default 0.9), so old habits
  fade and the coordinator keeps shifting.

### How it picks

For each receiver, a score per route:
1. **What works:** success rate from the brain, starting from an even guess so a new route isn't
   ruled out after one miss.
2. **The defense's memory:** `dbrain.c` keeps how familiar the defense is with each of 9 zones (3 depths
   x 3 sides). A route that ends in a zone the defense knows well scores lower. Each route's zone comes
   from its depth (from the route shapes in `ROUTES[]`) and the receiver's side.
3. **The situation:** routes too short for the yards to go score lower (long: no hitch or slant
   unless nothing else fits).
4. **A dash of surprise:** with chance `oc_surprise` (default 20%) a receiver gets a random allowed
   route instead of the best one.

The best score wins. The reason line names the biggest factor for the main call, for example
`OC: Post up top - their deep middle is unfamiliar` or `OC: Slant low - 4 of 5 worked on 3rd and short`.

### What the player sees

- The routes drawn before the snap are the coordinator's (the game draws routes set this way, as with
  hot routes).
- One line in the existing note spot (`O.note`, where `Hot: ...` shows today): `OC: ...`.
- A hot route after that replaces the coordinator's call for that receiver, as today.

## Tuning (Workshop Tune tab, the tune table)

| Name | Default | Meaning |
|------|---------|---------|
| `oc_on` | 1 | 1 = the coordinator calls routes, 0 = vanilla |
| `oc_surprise` | 20 | percent chance a receiver gets a random route instead of the best |
| `oc_fade` | 0.9 | how much of the old memory carries into each new game (1 = never forgets) |
| `oc_counter` | 1 | how much the defense's familiarity matters (0 = ignore it, 2 = double) |
| `oc_keep_pct` | 15 | percent chance a receiver keeps his vanilla route (variety from the game itself) |

## Testing

- The learning and picking math lives in its own file with no game calls (like `src/bullet.c`), with
  a test in `tests/` run by CI: a route that keeps working wins, the surprise rate holds, short routes
  aren't called on long yardage, a familiar zone loses to an unfamiliar one, the file round-trips.
- Log lines (`ocbrain: ...`) for each call and each lesson, so `/playtest` can show what it called,
  why, and what it learned.
- On the phone: one Extreme game. The log shows calls on every pass snap, lessons after throws, and the
  file saved at game end.

## Settled after the builder check

- **Crediting a throw:** at snap setup, keep a table `O.rcv[i] -> (route called, position)`. A hot route
  updates that receiver's entry. At the throw, look `P.rcv` up in that table; no match (0, the back, a
  lineman) teaches nothing.
- **Down and distance at the snap:** already read there (`offtools.c`, the `offtools: situation - ...`
  line). Use those values, not `dbrain.c`'s.
- **Route didn't take (`NOT APPLIED`):** the receiver keeps his vanilla route and the table records
  `keep` for him. Applied in C, so the play script's `route()` error doesn't apply.
- **Positions:** route runners other than the back, sorted top to bottom: the first is top, the last
  is bottom, any in between are middle. Duplicates are avoided only while unused routes remain.
- **Route zones:** a builder-made table (depth from the route shapes in `ROUTES[]`, side from the
  receiver's position, matching `dbrain.c`'s zone numbering). The log line for each call names the zone
  it assumed, so the first test game checks it.
- **Other difficulties:** off, with one log line per game saying why.
- **Saving:** after every lesson, like `dbrain.c` does, so a closed app loses nothing. Fade is applied
  once when a new game is detected.

## Builder's calls (not discussed with the player; change freely)

- Position buckets and situation buckets as above; decay per game rather than per play.
- The back is left alone.
- Score weights and how the reason line picks its factor.
- Whether keep is learned like any other route.
