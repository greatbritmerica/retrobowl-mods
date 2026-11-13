# Minute Drill Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A drive-long Minute drill mode in the play script that rewards on-tempo snaps (quick-catch yards, a juke prompt, harder run blocking), backed by three new script tools in the mod build.

**Architecture:** The mod build adds `tempo()`, `tackler()`, `juke_ready(player)`, and the events `on drive_end` and `on juke`. The juke prompt is a native overlay panel (`rb_ui_panel`, a 4th panel id), so its tap is a real iPhone button press in any phase, not game input. The closing check is pure math in `src/juke.c`, tested in CI. The script section replaces stop and go.

**Tech Stack:** C (iOS dylib via GitHub Actions), the play-script language (`docs/SCRIPTING.md`), the Workshop checker (`site/script.js`, run by `tools/check_script.js`).

**Spec:** `docs/superpowers/specs/2026-11-13-minute-drill-design.md`

## Global Constraints

- A new script tool is listed in 4 places: `src/script.c` (enum `F_*`, `FN[]` table with arg counts, evaluator case), `site/script.js` (`FNS`, with a help line), `docs/SCRIPTING.md` (table row), and its C implementation in `src/offtools.c` (`sh_*`). A new event is listed in `src/script.c` `EVENTS[]`, `site/script.js` `EVENTS`, and `docs/SCRIPTING.md`.
- Tunables in `tools/tunables.json`, group "Minute drill": `juke_reach` 2.5 (yards, 1-5, step 0.5), `juke_window` 0.6 (seconds, 0.2-1.5, step 0.1). Generated files are rebuilt by the hook: never edit them.
- Script tunables (top of the section): `md_on` 1, `md_window` 10, `md_quick` 1.5, `md_short` 5, `md_slow_pct` 60, `md_slow_n` 2, `md_juke_yards` 5, `md_juke_push` 3, `md_block_pct` 70.
- Messages: `Minute drill: snap fast!`, prefixed `Last snap too slow. ` after a slow snap.
- The script stays within 48 blocks / 64 variables alone and with the Hail Mary (`tools/check_script.js`).
- `speed()` once per player per play. A handoff is ball `state() == 19`.

## Review Focus

- A juke armed but nobody ever closes (a catch near the sideline, run out of bounds): the panel never shows, nothing stays armed into the next play (Task 2: arm cleared in `rb_off_forget` and at each snap).
- The JUKE tap landing after the window or after the play ends: ignored, no `on juke` (Task 2: callback checks the window and that the play is live).
- A drive that ends by a CPU score after your turnover, a kickoff return or a safety: `drive_end` fires once for the player's lost possession, never on the CPU's (Task 2: fires only when possession goes from the human team to the other).
- `tempo()` on the first play of a game or after the app restarts: -1, and the script treats it as on tempo only on the snap that turns the mode on (Task 3).
- A pass caught by someone the script didn't expect (the back, a tight end): rewards still use the nearest offense player to the ball, never nil (Task 3: `nearest(offense(), ...)` and nil checks).

---

### Task 1: Juke closing check (pure, CI tested)

**Files:** Create `src/juke.h`, `src/juke.c`, `tests/juke_test.c`. Modify `Makefile` (SRC and headers), `.github/workflows/build.yml` (test step after the coordinator test).

**Interfaces (Produces):**
```c
// 1 when the defender is within reach of the carrier and their gap is shrinking (px and px/frame).
int rb_juke_closing(double cx, double cy, double cvx, double cvy, double dx, double dy, double dvx, double dvy, double reach);
```

- [ ] **Step 1: Test** `tests/juke_test.c` (asserts, prints `juke_test ok`):
  - head-on: carrier (0,0) v(2,0); defender (40,0) v(-2,0); reach 51 → 1.
  - out of reach: defender at (80,0) → 0.
  - chasing from behind but slower: carrier v(3,0), defender (-30,0) v(2,0) → 0 (gap growing).
  - chasing from behind and faster: defender v(4,0) → 1.
  - side by side, same velocity: defender (0,30) v(2,0) carrier v(2,0) → 0.
- [ ] **Step 2:** Implement: distance <= reach and the relative velocity points toward the carrier (dot((c - d), (dv - cv)) > 0).
- [ ] **Step 3:** Add to Makefile and CI; commit `Juke closing check (pure, tested in CI)`; push; the CI run must end `success`.

### Task 2: Script tools and events in the mod build

**Files:** Modify `src/script.c`, `site/script.js`, `docs/SCRIPTING.md`, `src/offtools.c`, `src/offtools.h`, `src/hooks.c`, `src/ui.h`, `tools/tunables.json`.

**Interfaces:**
- Consumes: `rb_juke_closing` (Task 1).
- Produces for scripts: `tempo()` → number; `tackler()` → player or nil; `juke_ready(player)` → nil; `on drive_end`; `on juke`.

- [ ] **Step 1: tempo().** In `offtools.c` keep a frame counter that runs every `rb_off_step` call (ball step). Record the frame when the ball goes dead on your play (the states the existing code treats as play over: 4, 6, 7, 9, 11) and at the snap set `O.tempo = (snap frame - dead frame) / 60`, or -1 with no dead frame this game. `sh_info("tempo")` returns it. Log `md: tempo %.1f s`.
- [ ] **Step 2: juke_ready / tackler / on juke.** `#define RB_PANEL_JUKE 3` in `ui.h`. `juke_ready(p)` stores the player for this play (cleared at each snap and in `rb_off_forget`). Each frame while armed and no juke used: track the carrier's and each defender's velocity from last frame's position (as `super_track` does), and pick the nearest defender for which `rb_juke_closing(..., T(T_JUKE_REACH) * 20.5)` is 1. On the first such frame show the panel (`rb_ui_panel(RB_PANEL_JUKE, RB_SIDE_RIGHT_BOTTOM, 124, "", "", {"JUKE"}, 1, cb_juke)`), remember the tackler and the frame, log `md: juke shown (tackler %.0f px away)`. Close it after `T(T_JUKE_WINDOW)` seconds or when the play ends (log `md: juke missed`). The callback sets a flag; the next `rb_off_step` (game thread) fires `rb_script_fire("juke", 0)` if inside the window and the play is live, logs `md: juke tapped`, closes the panel, disarms. `tackler()` returns the stored tackler (nil otherwise; add it to `sh_one`).
- [ ] **Step 3: on drive_end.** In `hook_change_possession` (`hooks.c`), when possession goes from the human team (`SLOT_PLAYERTEAMINDEX`) to the other, fire `rb_script_fire("drive_end", 0)` and log `md: drive end (possession)`. In `offtools.c`'s presnap block, when `rb_rctx.quarter` goes from 2 to 3 (store last quarter), fire it too with `md: drive end (half)`.
- [ ] **Step 4: List them.** `F_TEMPO`, `F_TACKLER`, `F_JUKE_READY` in the enum and `FN[]` (`{"tempo",0,0}`, `{"tackler",0,0}`, `{"juke_ready",1,1}`); evaluator cases (tempo through `info`, tackler through `sh_one`, juke_ready through a new `sh_juke_ready(void *p)`). `"drive_end"`, `"juke"` in `EVENTS[]`. Same in `site/script.js` (FNS with help lines, EVENTS) and `docs/SCRIPTING.md` rows. Tunables `juke_reach`, `juke_window`.
- [ ] **Step 5:** Run `"C:/Program Files/nodejs/node.exe" tools/check_script.js examples/step1_script.txt` → still loads (no script change yet). Run the `mod-pitfall-reviewer` on the diff, fix what it finds. Commit `Script tools for the minute drill: tempo(), juke prompt (juke_ready, on juke, tackler), on drive_end (v93 mod build)`; push; CI `success`.

### Task 3: The Minute drill script section

**Files:** Modify `examples/step1_script.txt` (replace `# 3. STOP-AND-GO` through the end of its `on frame` block). Keep section 2's `if not (scr_ready and r == back())` style checks independent of the removed `sng_*` names (grep for `sng_` afterwards: none left).

**Interfaces:** Consumes Task 2's tools and events.

- [ ] **Step 1:** Write the section to the spec's Flow, with the Global Constraints tunables and messages. Handlers: `on presnap` (button, message when the mode is on), `on snap` (button pressed → mode on and this snap on tempo; else on tempo = `tempo() >= 0 and tempo() <= md_window`; log `s1: md snap tempo <t> on <0|1>`), `on throw` (store `since_snap()`), `on catch` (on tempo only; catcher = `item(nearest(offense(), ball().x, ball().y, 1), 0)`; depth = `(ball().x - los_x()) * fwd() / yard(1)`; quick and short → slow the `md_slow_n` nearest defenders; depth > `md_juke_yards` → `juke_ready(catcher)`; log each), `on juke` (push `tackler()` `md_juke_push` yards away from the ball for 0.4 s in `on frame` via a stored target; `speed(tackler(), md_slow_pct)`; log), `on frame` (the push; on tempo and `state() == 19`: each receiver steered to his nearest defender, that defender slowed to `md_block_pct` once when within a yard), `on drive_end` (mode off, log `s1: md off`).
- [ ] **Step 2:** `tools/check_script.js` → loads, within 48/64 alone and with the Hail Mary. If over 64 variables, drop one-shot log flags (like `scr_logged`) first.
- [ ] **Step 3:** `mod-pitfall-reviewer` on the script diff; fix findings. Commit `Minute drill replaces stop and go: tempo mode with quick-catch yards, juke prompt and run blocking`; push.

### Task 4: v93 for the phone

- [ ] **Step 1:** Final whole-branch review (fresh reviewer) of Tasks 1-3; fix Critical/Important.
- [ ] **Step 2:** CI artifact → swap into `RetroBowl-1.6.49-v92.ipa` as `RetroBowl-1.6.49-v93.ipa` in iCloud "Retro Bowl mods"; `bash tools/deploy_script.sh`.
- [ ] **Step 3:** Tell the user: install v93, move the script, play Extreme, try the coordinator, the screen and the minute drill; "log's up".
