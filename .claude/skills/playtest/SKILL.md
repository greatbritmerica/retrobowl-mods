---
name: playtest
description: Review a Retro Bowl playtest from rbmod.log - what each script feature did, script errors, and pictures of traced plays. Use when the user says the log is up, sends rbmod.log, or asks how a test game went.
---

# Playtest review

The log is `~/iCloudDrive/Retro Bowl mods/rbmod.log` unless the user gives another path. It holds every
session ever played (several MB): only look at the part after the last script load.

1. **Find the last load:**
   `grep -n "rbmod_script.txt loaded\|NOT loaded" "$LOG" | tail -3`. Take the line number of the last
   `loaded` (or report the `NOT loaded` reason and stop). If the user says they played across restarts,
   start from the earlier load they mean.
2. **Pull what the scripts did** from that line on, without the trace lines:
   `tail -n +$N "$LOG" | grep "s1: \|error on line\|is off until" | grep -v "s1: T[BP]"`.
   An `error on line` / `is off until` line means a block switched itself off: always report it first.
3. **Draw the traced plays:** count `s1: TS` lines after the load, then
   `python tools/trace_plot.py "$LOG" "<scratchpad>/traces" --last <count>`. Read the PNGs that matter
   (offense always attacks to the right; ring = start, dot = end, white dots = ball) and send the
   telling ones to the user.
4. **For a play that went wrong, read the game's side** around it:
   `grep -n "\[<timestamp>\] script: s1: <feature>" "$LOG"`, then the next few hundred lines filtered to
   `dbrain:\|offtools:\|Tipped\|Incomplete\|Dropped\|INTERCEPT`.
5. **Report per feature** (super throw, screen, stop and go, injuries, anything new): what the user said,
   what the log and picture show, the cause, and the proposed change. Propose, don't edit: the user
   approves changes first.

Known facts that explain most surprises (see CLAUDE.md): catch / incomplete / intercepted / touchdown /
play_end events and landing_x() only happen on Extreme; play_end does not run after a sack; at most 7
rule/script buttons show before the snap (3 before v96).
