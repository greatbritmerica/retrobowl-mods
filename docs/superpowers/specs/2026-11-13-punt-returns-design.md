# Punt returns - design

Status: approved by the user, 2026-11-13 (approach B). Needs C (`src/`) and a new IPA.

## What and why

When the CPU punts to the user's team, the user fields it and runs it back, just as they already do on
kickoffs. The user asked for it to work "off of what's already there for kickoff, tweaked slightly".
They don't want a new punt game; they want the kickoff return they know, on punts.

## How the game works today (from the 2026-11-13 `rbmod.log`)

- **Kickoffs** are a live play. The game logs `s_get_player_kick_returner` ("Override with a returner:
  x = 2400, y = 300"), then the kick runs live (ball state 5) and `s_receive_ball` hands it to the
  returner, who the user steers.
- **CPU punts** aren't a play. `S_ACTION_RESULT: ACTION_RESULT_PUNT`, then `s_punt`, then
  `s_change_possession` set the new scrimmage spot in the same frame (`scrimmage = -24`). There's no
  kick, no returner and no coverage. All 16 CPU punts in that log went this way, on both simmed CPU
  drives and the drives the mod plays out (`cpuqb` presses `btn_response_punt_yes`, `hooks.c`).
- **The mod has no return code** apart from interception returns (`int_return_fix` in `hooks.c`).
- **Names** (`src/script_names.inc`): `s_punt` 0x4ce980, `s_received_AI_kickoff` 0x4d1620,
  `s_kick_off` 0x4308f8, `s_get_player_kick_returner` 0x3cbf04, `s_receive_ball`,
  `btn_click_OptionsKickReturns`.

## Approaches

- **A. Swap the punt for the game's kickoff-return play.** When the CPU punts to the user, skip the
  instant punt, start the AI-kickoff play, move the kick's start to the punt spot and shorten it to
  punt distance. The return ends where the returner is down. The game will treat it as a kickoff:
  commentary may say kickoff, and the coverage lines up like a kickoff.
- **B. A, plus an on/off setting and a safety net. Chosen.** If the swap fails, the normal instant
  punt runs instead, so a game never stalls. The user can only test in real games, so a failure must
  cost one return, not a game.
- Dropped: a random "return yards" bonus with no live play (the user wants to run it), and a real punt
  formation with a snap and gunners (far more work than the user asked for).

## Behaviour

1. The CPU punts on 4th down (simmed or played drive). The mod catches the game's punt routine (`s_punt`)
   only when the punting team is the CPU and the setting is on.
2. Instead of the instant spot, the game's AI-kickoff return play starts. The kick comes from the CPU's
   line of scrimmage and travels about `PUNT_DIST` yards, give or take `PUNT_SPREAD`, toward the user's
   end zone.
3. The returner is placed under the ball (the game's own returner override, moved to the landing spot).
   The user catches it and runs, with the same blocking and coverage as a kickoff today.
4. Where the returner goes down is where the user's drive starts. A touchdown is a touchdown. A kick
   that would land in the end zone is a touchback at the 20, with no live play (my call; it matches a
   real NFL touchback, and a kickoff return from deep in the end zone would make no sense on a punt).
5. **Safety net:** if the swap can't be set up (a routine missing, the play not starting within a few
   frames, a punt from a spot the code doesn't expect), the original `s_punt` runs unchanged and the
   log says why.

## Settings (top of the C section, plain-English comments, in the tune settings file)

| Name | Default | Meaning |
|---|---|---|
| `PUNT_RETURN` | 1 | 1 = you return CPU punts, 0 = the game's instant punt |
| `PUNT_DIST` | 42 | average punt distance in yards |
| `PUNT_SPREAD` | 8 | punts land up to this many yards shorter or longer |

These are my numbers; the user can tune them. If the Workshop page shows settings, add these to it.

## Non-goals

- No fair catch button (kickoffs don't have one).
- No muffed punts.
- No punt formation, gunners or punt blocks; no change to the coverage AI.
- The user's own punts are out of scope. Those belong to the session
  [Punting: player controls the punt](#local_3e26ebed-ee75-4ee0-bc49-0f8389dff2b6).

## Anti-goals (a failure even if it technically "works")

- A game that hangs or crashes on a CPU punt. That's why the safety net exists.
- The user's own punts changing in any way.
- Field position that's wrong after the return (the drive starting somewhere other than where the
  returner went down).

## Constraints and risks

- Must not touch the user's punts: act only when possession is the CPU's. The punting session may hook
  `s_punt` too. Both changes must share one hook and branch on who is punting.
- My call: if the game's own "Kick Returns" option is off (kickoffs are auto-returned), punts stay
  instant too.
- **Biggest unknown:** whether the AI-kickoff play can start from any spot. The game may set the kick's
  start and the scrimmage after a kickoff from fixed numbers (the kicking tee at x ~2206,
  `PX_PER_YD` 20.5). The builder has to find where the kickoff sets the ball's start, landing and
  final scrimmage, and set those from the punt line. If it can't be moved, the safety net keeps the
  instant punt, and the log has to show which step refused.
- Simmed CPU drives: kickoffs after simmed drives are already live, so a live return after a simmed
  punt should be fine. It's still the first thing to check in the log.
- The game clock may run differently on a kickoff. That's acceptable unless the test shows something
  odd.
- Commentary may say "kickoff". Acceptable for the first version.

## Left to the builder

- How to start the kickoff-return play from inside the punt hook (call `s_received_AI_kickoff`, or set
  the state the game checks and let the next frame start it). Read the routine's disassembly first.
- Where the kick's start, target and the after-return scrimmage live, and the field-coordinate maths.
- The log lines for `/playtest`: `punt return: CPU punt from own N, kick to M, returned to K` and every
  fallback with its reason.

## What building it takes

1. Read the disassembly of `s_punt`, `s_received_AI_kickoff`, `s_kick_off` and
   `s_get_player_kick_returner` to find the start, target and scrimmage values. No game time needed.
2. C in `src/`: a new `HOOK(punt, ...)` line in `src/hooks.def` (or a shared one with the punting
   work), a small `puntret.c`, and three tune values in `tune.h`.
3. Run the `mod-pitfall-reviewer` agent, then push to `step1-scripts`; CI builds `libRBMod.dylib`; a
   new IPA swaps the dylib in.
4. One test game on Extreme with a few CPU punts. The user sends `rbmod.log`, and `/playtest` checks
   the punt-return lines, the field position and any fallbacks.
5. Expect one more IPA if the first test shows the kick can't be moved and the fallback took over.
