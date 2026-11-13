# Coin toss: call heads or tails (design)

Date: 2026-11-13. Status: approved by the user 2026-11-13. Not built yet.

## Why
The game flips the opening coin on its own. The user wants the feel of a real toss (call it, see it land)
and, on a win, the choice to kick or receive.

## Goals
- Before the opening kickoff, an in-game side panel asks: Heads / Tails.
- A short "Flipping..." pause, then the result.
- Right call: "HEADS - you win the toss!" with [KICK] [RECEIVE]. The choice is used.
- Wrong call: "It's TAILS. <Opponent> won the toss and will receive." Then the game carries on, with the user kicking.

## Non-goals
- No change to the second half (the game already swaps who kicks).
- No overtime toss (unless the same pop-up shows there anyway; see Open points).
- No coin animation or art (the button restyle is a separate task).
- No on/off switch (nobody asked for one).

## Anti-goals
- A toss that hangs, or a game that never kicks off. If anything is unexpected, the game's own toss stands.

## Constraints
- **Needs a mod build and a new IPA.** Script blocks only run from just before a snap onward, and the toss
  happens before the first play. So this can't be done with a script alone. It can ride along in any build
  that's already planned.
- The panel callback runs on the iPhone's main (UI) thread. The game's routines must run on the game's thread.
  Write the answer there and apply it from the game thread, as `src/defcall.c` does (`g_answer`, `on_call`).

## Approaches considered
1. **Take over the game's toss messages (picked).** `hook_create_message` (`src/hooks.c`) already reads every
   pop-up's text. When it's a toss pop-up, hide it, run our own flip in a `rb_ui_panel` side panel, then run
   the game's own answer: `btn_response_cointoss_kick` / `btn_response_cointoss_receive` (both in
   `src/script_names.inc`). Small, and it reuses working parts. Risk: the "you lost the toss" pop-up hasn't been
   seen yet.
2. **Rig the game's flip so the user always wins, then put heads/tails in front of the game's own question.**
   Keeps the game's look for kick/receive, but first we'd have to find where the game flips. That's slow,
   unknown digging. Not picked.
3. **Skip the toss and set the kicking team directly** (`kickoffteam`, `s_set_up_kickoff`). Riskiest: a
   mistake breaks the opening kickoff. Not picked.

## Design (approach 1)
- **Spot the toss pop-ups** in `hook_create_message` by their text. The win question mentions kick/receive.
  The lose notice text is unknown. Log every pop-up's text before the first kickoff so one test game shows
  both.
- **When one shows:** don't show it (skip `ORIG`, or close it right away with the game's message-clear routine;
  pick whichever works). Open a panel (new panel id, left side, the same style as the defense calls):
  title "COIN TOSS", text "Call it:", buttons [HEADS] [TAILS].
- **Flip:** a 50/50 coin. Change the panel to "Flipping..." with no buttons for `TOSS_FLIP_SECONDS`, then show
  the result.
- **Win:** the panel shows "<SIDE> - you win the toss!" with [KICK] [RECEIVE]. On the game thread, run
  `btn_response_cointoss_kick` or `_receive`.
- **Lose:** the panel shows "It's <SIDE>. <Opponent> won the toss and will receive." with [OK]. Then run
  `btn_response_cointoss_kick` (the user kicks).
- **Fallback:** if the toss pop-up text isn't recognised, change nothing: the game's own toss stands.
  Log it.
- **Tunables** (top of the new C section, each with a plain-English comment):
  `TOSS_FLIP_SECONDS` (default 1.0), the panel width.

## Open points (the first test game settles them)
- What the game shows when the user loses its toss. If it shows no pop-up, those games keep the game's own
  toss. A follow-up would then hook the toss decision itself.
- Whether `btn_response_cointoss_*` works when the game's own question was never shown (or was closed).
  The log should show who kicks off.
- Whether the same pop-up shows in overtime. If so, the panel will show there too. That's fine, but say so
  to the user.

## Left to the builder
- Where the new code lives (a small `toss.c`, or inside `hooks.c`) and the panel id.
- Hide the pop-up by skipping `ORIG` or by clearing it, whichever the game accepts.
- The opponent's name: read it from a game value if one is handy, else "The opponent".
