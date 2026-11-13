# Weekly practice — design

Date: 2026-11-13 · Status: approved by the user 2026-11-13 · Needs: C and a new IPA (two builds)

## Why
To give the season more life: the week between games should feel like a team's week, not a menu skip.
It is atmosphere first; the stat effect is a small bonus, not a power-up.

## What the user sees
Once a week, before the next game, a popup:

```
      WEEK 6 PRACTICE
 What's the focus this week?
 [ Weight room ]  [ Tackling ]
 [ Running ]      [ Routes ]
 [ Skip ]
```

Tap one → a one-line result, worded differently week to week, e.g.
"Good week in the weight room — the line looks a bit stronger." Then the week carries on as normal.

## Decided with the user
- **Picked from a menu**, not played as minigames.
- **Upside only**: no fatigue, injuries or other cost.
- **Ratings are raised through the game's own routine** (the one used when spending coaching credits on a
  boost), so the game's caps and rounding still apply.

## My calls (the user can correct these)
- Every week of the season, playoffs included; Skip is always there; one focus per week.
- Drill → group: Weight room → linemen (OL/DL) strength-type rating; Tackling → defenders' tackling-type
  rating; Running → RBs; Routes → WRs/TEs. Exact rating names come from the detective build (below).
- Boosts are small: tunable, default one rating point to each starter in the group.
- A handful of result lines per drill, picked at random.

## Goals / non-goals / anti-goals
- Goal: a quick weekly beat (about 5 seconds) that reads like a team report.
- Non-goals: played drills, a passing drill (the game already has passing practice), morale or chemistry systems.
- Anti-goals: practice that unbalances the season; anything that can damage a save; slowing down the
  week-to-week flow.

## Constraints
- Scripts only run during plays, so the whole feature is C in `src/` → a new `libRBMod.dylib` → a new IPA.
- The mod can already run any game routine by name (`sh_call` in `src/script_game.c`), show a popup with
  several buttons (`rb_ui_choice_n` in `src/ui.c`), and hook game routines (`src/hooks.def`).
- Unknowns: what the "raise rating" routine (`s_increase_player_attribute`) expects (which player, which
  rating, how much), how the roster is stored, which instance it must run as, and whether the "next week"
  moment (`s_do_next_week` / `s_play_endweek`) is a safe place to show a popup.

## Approaches considered
1. **Game's own "raise rating" routine** — chosen. It's faithful to the game and safe. It needs a detective build first.
2. Game's "XP to whole team" routine (`s_add_xp_gain_all`): simpler, but every drill does the same thing,
   which kills the feel. This is the fallback if per-group boosts prove impossible.
3. Writing rating numbers into the roster directly: full control, but a wrong spot could corrupt a save. Rejected.

## Building it
1. **Detective build (IPA 1).** Hook `s_increase_player_attribute`, `s_do_next_week` and `s_play_endweek`. Log
   their arguments and the calling instance. Find the roster (players, positions, rating names) and log it once.
   The user plays a game, buys one skill boost with coaching credits, advances a week, and sends `rbmod.log`.
2. **Practice build (IPA 2).** At the safe "next week" moment, show the popup. On a pick, call the raise routine
   once for each player in the group, then show the result line. Tunables at the top of the C section, with
   plain comments: boost size, players per group, show in playoffs, result lines.
3. Test game: pick each drill once across four weeks and check the ratings in the roster screen.

## Left to the builder
How to find the roster and position groups (from the detective log); the exact hook point; how to make the
popup appear only once per week (it should not reappear after leaving and reopening the app mid-week, if that
can be detected cheaply).
