# Punts kicked like field goals (v101) - design, waiting for the user's yes

User (2026-11-15, after the v100 test): the punt must not use the QB throw. Use the kickoff / field goal
kick for the start, then watch the ball travel and end one of these ways: lands in the field, in the end
zone, out the back of the end zone, caught by the returner, or on the ground and picked up by the kicking team.

## What the research found (v100 binary, capstone)
- **Your kickoffs are instant** (log: `s_kick_off` -> `scrimmage = -45`, no kick on screen). Only field
  goals and the CPU's kickoffs to you show a kicker. So the field goal kick is the model.
- `s_set_up_fieldgoal` (0x548...) sets `kicking`, `x_scrim`, `kick_dist`, `state`, commentary and the
  `training_Kick1` text; it uses the normal Players and Ball.
- `s_kick_ball` (0x10042e3xx) is the release: `s_set_anim` on the kicker (the kick animation), then the ball
  gets `velocity` from `kickpow` (x1.5) and `zvel` (x0.5), aim from `direc`. The flight is the ball's own
  `s_update_ball_movement` physics; `s_update_ball_fieldgoal` runs the swipe meter (`kickpow`, `direc`,
  `kickflip`, `my_kick_skill`) and later the good / no good result.
- The mod already drives that swipe for the CPU (`cpuqb.c` PH_FG) and logs `kick f.. kickpow direc flip`.
- Interception returns are live in this game (the defender runs, `PURSUIT_BURST` in hooks.c): the basis for
  a CPU punt return.
- `btn_response_fg_yes` (script "Field goal" button, v96) has never been pressed in any log: untested.

## Design
1. Punt button on 4th down -> the game's field goal set-up instead of a scrimmage play (flag: punt).
2. You swipe as for a field goal: the game's kicker runs up and kicks.
3. At the kick the mod takes the ball over: distance from the swipe power (punt_min..punt_max yards), aim
   from the swipe, the existing on-screen flight (height arc) - no good/no good result.
4. Your 10 others run downfield to cover; one of their players stands at the landing spot.
5. Endings: in the field and untouched -> rolls a few yards, their ball there; in or through the end zone ->
   touchback, their 20; out of bounds -> their ball where it crossed; caught -> live return (as an
   interception return), their ball where he's down, touchdown if he scores; muffed (chance, default 5%)
   -> loose ball, nearest player after a short roll gets it (yours = your ball, 1st and 10).

## Unknowns, checked in this order while building
- Can the field goal set-up start from deep in your own end (a 90-yd "field goal")?
- Can the game's good/no good result be skipped after the kick without crashing (same alarm-0 trap as v99)?
- Can a defender be handed the ball so the game runs it as an interception return in a kicking play?
- Interception stats may count against your QB on a caught punt.

## Built (v101): the regular-formation version
The user picked the regular formation once it turned out a field goal play gives the mod no ball step (the
log shows no `ball f..`/`kick f..` lines from the kick set-up to "It's good!"), so no live return there.
- Punter (QB) plays `s_set_anim(15)` at the release; the ball flight is the mod's (`punt.c`).
- Catch: `s_receive_ball` run as the returner (sets holder, ball state 5), then state 9 = the game's
  interception return (hooks.c `int_return_fix` chases him). Muff: `s_drop_ball(true)` as him.
- `s_check_ball_collision` has the game's own branches for a kickoff catch, an interception and a fumble
  recovery ("FUMBLE CATCH" / "match_Recovered" / "FUMBLE TURNOV").
- Check in the v101 log: `punt: punter's kick animation (sprite a -> b)`, `caught by the returner ... ball
  state x -> 5 -> 9`, `punt return: result N ...` and the `action_result done: possession` line after it.
