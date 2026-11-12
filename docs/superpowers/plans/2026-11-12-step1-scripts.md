# Step 1 Play Scripts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One script file, `examples/step1_script.txt`, that adds a super bullet throw, a screen pass, a stop-and-go button with a ball-holder probe, and light injuries, all with plain-English tuning numbers at the top.

**Architecture:** Plain text in the v89 play-script language (`docs/SCRIPTING.md`). Four labeled sections, each opening with an `on presnap` block of tuning variables and an on/off switch. No C, IPA or Workshop changes.

**Tech Stack:** The play-script language. The Workshop checker `site/script.js` (`WS_SCRIPT.check(text)` returns `{ok:true, handlers:[...]}` or `{line, msg}`).

**Spec:** `docs/superpowers/specs/2026-11-12-step1-scripts-design.md`

## Global Constraints

- Needs mod build v89 or later. Only `rbmod_script.txt` is read by the game.
- Engine limits: at most 48 `on` blocks, 64 variables, 32 players in a list, 20,000 steps per block run (4,000 in `on frame`).
- Every tuning number sits in its section's first `on presnap` block with a `#` comment in plain English.
- Every section has an on/off variable: `super_on`, `screen_on`, `sng_on`, `probe_on`, `injury_on` (1 = on, 0 = off).
- Every log line starts with `s1:`.
- `msg()` and `button()` only in `on presnap`. `carry()` only in `on release`.
- A run error switches off only its own `on` block, so anything that reads an unconfirmed game value gets a block of its own.

## Review Focus

1. **Pasted together with the Hail Mary script:** the file must still load and stay within 48 blocks and 64 variables. Task 5 checks the combined file.
2. **Feature switched off (`*_on = 0`):** no button, no message, no effect, no log line. Every block's first line is `if <x>_on` (or contains it).
3. **Two buttons pressed on one play** (Super throw + Screen): both run, and nothing errors. Task 5 reads every `on frame` block for shared variable names.
4. **New game started:** the meter carries over on purpose, and injuries clear. Task 4 resets on `quarter()` going down.
5. **Reading an unconfirmed value (`ball().holder`, player identity names):** may cause a run error. It must sit alone in its block so only the probe goes off.

## How to check a script here

There's no Node on this PC. Use the built-in browser pane's `javascript_tool` on any open tab: evaluate the text of `site/script.js` (it sets `window.WS_SCRIPT`), then `WS_SCRIPT.check(<file text as a JSON string>)`. Build that call with:

```bash
python -c "import json;print('(function(){'+open('site/script.js',encoding='utf8').read()+'})();WS_SCRIPT.check('+json.dumps(open('examples/step1_script.txt',encoding='utf8').read())+')')"
```

and pass the output as the `text` of `javascript_tool`. **Pass** = `{"ok":true,"handlers":[...]}` with the handler list the task names.

Limits check (blocks and distinct assigned names):

```bash
python -c "import re,sys;t=open(sys.argv[1],encoding='utf8').read();t=re.sub(r'#.*','',t);print('blocks',len(re.findall(r'^\s*on\s',t,re.M)),'vars',len(set(re.findall(r'^\s*([A-Za-z_]\w*)\s*=[^=]',t,re.M))|set(re.findall(r'^\s*for\s+(\w+)',t,re.M))))" examples/step1_script.txt
```

---

### Task 1: File header and super bullet throw

**Files:**
- Create: `examples/step1_script.txt`

**Interfaces:**
- Produces: the file, with a header comment (what it is, paste into the Script tab, needs v89) and section 1. Variables: `super_on`, `meter_per_catch`, `meter_per_miss`, `super_needs`, `super_slow_defenders`, `super_def_speed`, `meter`.

- [ ] **Step 1: Write section 1** with these blocks, in order:
  - `on presnap`: tuning. `super_on = 1`, `meter_per_catch = 20`, `meter_per_miss = 10`, `super_needs = 100`, `super_slow_defenders = 1`, `super_def_speed = 70`. If `meter == nil`, set it to `0` (variables persist across plays).
  - `on presnap`: if `super_on`, `msg("Super meter " + str(min(100, floor(meter * 100 / super_needs))) + "%")`, then if `meter >= super_needs`, `button("Super throw")`.
  - `on catch` / `on incomplete` / `on intercepted`: if `super_on` and not `pressed("Super throw")`, add or subtract, clamp to 0..100, and `log("s1: meter", meter)`.
  - `on snap`: if `pressed("Super throw")`: `tune("pick_base", 0)`, `tune("pick_known", 0)`, `log("s1: super throw")`.
  - `on frame`: if `pressed("Super throw")` and `thrown()`: steer the one receiver from `nearest(receivers(), landing_x(), landing_y(), 1)` to the landing spot at strength 1. If `super_slow_defenders`, call `speed(d, super_def_speed)` for each `d` in `nearest(defenders(), landing_x(), landing_y(), 2)`.
  - `on play_end`: if `pressed("Super throw")`: `meter = 0`.
- [ ] **Step 2: Check it.** Expected handlers: `presnap, presnap, catch, incomplete, intercepted, snap, frame, play_end`.
- [ ] **Step 3: Commit:** `git add examples/step1_script.txt && git commit -m "Step 1 scripts: super bullet throw"`

### Task 2: Screen pass

**Files:** Modify: `examples/step1_script.txt` (append section 2).

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `screen_on`, `screen_side`, `screen_depth`, `screen_linemen`, `screen_time`, `scr_y` (the chosen side's target y), `scr_x`.

- [ ] **Step 1: Write section 2:**
  - `on presnap`: tuning. `screen_on = 1`, `screen_side = 0` (1 top, -1 bottom, 0 the side with more room), `screen_depth = 3`, `screen_linemen = 3`. If `screen_on`, `button("Screen")`.
  - `on snap`, if `pressed("Screen")`:
    - Pick the side. With `screen_side = 0`, compare `back().y - field_top()` with `field_bottom() - back().y` and take the larger. Set `scr_y` to `back().y` moved `yard(8)` toward that side.
    - Set `scr_x = los_x() - fwd() * yard(screen_depth)`.
    - `route(back(), "route_flat")`, then log the side.
  - `on frame`, if `pressed("Screen")` and `since_snap() < screen_time`. `screen_time = 3` is one more tuning line in the presnap block. Don't use `state()` here: the QB holding the ball already counts as "someone has it".
    - Loop over `nearest(offense(), los_x(), back().y, screen_linemen + 3)`. Skip `qb()`, `back()` and any player equal to a member of `receivers()` (inner `for` with `==`).
    - Steer the first `screen_linemen` that remain to `scr_x, scr_y` with strength 0.8.
  - Log who was picked, once, at the first frame: `log("s1: screen blocker", p.x, p.y)`.
- [ ] **Step 2: Check it.** Expected new handlers: `presnap, snap, frame`.
- [ ] **Step 3: Commit:** `git commit -am "Step 1 scripts: screen pass"`

### Task 3: Stop-and-go and ball-holder probe

**Files:** Modify: `examples/step1_script.txt` (append section 3).

**Interfaces:** Produces: `sng_on`, `probe_on`, `last_holder`, `last_state`.

- [ ] **Step 1: Write section 3:**
  - `on presnap`: `sng_on = 1`, `probe_on = 1`, `last_state = -1`, `last_holder = -1` (so the first frame of every play logs, and nothing compares against `nil`). If `sng_on`, `button("Stop and go")`.
  - `on snap`: if `pressed("Stop and go")`, `route(r, "route_slant_fake")` for every receiver, and `log("s1: stop and go")`.
  - `on frame` (probe state): if `probe_on` and `state() != last_state`: `log("s1: ball state", state(), since_snap())`, then `last_state = state()`.
  - `on frame` (probe holder, its own block because `ball().holder` may not be a number): if `probe_on` and `ball().holder != last_holder`: `log("s1: holder", ball().holder, since_snap())`, then `last_holder = ball().holder`.
- [ ] **Step 2: Check it.** Expected new handlers: `presnap, snap, frame, frame`.
- [ ] **Step 3: Commit:** `git commit -am "Step 1 scripts: stop-and-go and ball-holder probe"`

### Task 4: Light injuries

**Files:** Modify: `examples/step1_script.txt` (append section 4).

**Interfaces:** Produces: `injury_on`, `injury_chance`, `injury_speed`, `injury_sacks`, `hurt_x`, `hurt_y`, `hurt_id`, `hurt_new`, `last_q`, `no_tackle`.

- [ ] **Step 1: Write section 4:**
  - `on presnap`: tuning. `injury_on = 1`, `injury_chance = 3`, `injury_speed = 85`, `injury_sacks = 1`.
    - **New game:** if `last_q != nil` and `quarter() < last_q`, set `hurt_id = nil`. Then `last_q = quarter()`.
    - If `hurt_new`, `msg("Injury: a player is hobbled for the rest of the game")` and `hurt_new = 0`.
  - `on presnap` also sets `no_tackle = 0`. `on incomplete` and `on touchdown` set `no_tackle = 1`, so those plays can't injure anyone.
  - `on play_end`: if `injury_on` and not `no_tackle`, `p = item(nearest(offense(), ball().x, ball().y, 1), 0)`. If `chance(injury_chance)`: `hurt_x = p.x`, `hurt_y = p.y`, `hurt_new = 1`, `log("s1: injured at", p.x, p.y)`.
  - `on sack`: the same with `qb()`, only if `injury_sacks`.
  - **Identity probe.** One `on play_end` block per candidate name, each run only when `hurt_new`. Each logs `log("s1: injured id <name>", get(p, "<name>"))` for the player nearest `hurt_x, hurt_y`. Candidates, in order: `id`, `player_id`, `uid`, `number`, `slot`. The first block also sets `hurt_id = get(p, "id")`.
  - **Effect.** `on snap`: if `injury_on` and `hurt_id != nil`, then for each `p` in `offense()` with `get(p, "id") == hurt_id`: `speed(p, injury_speed)` and `log("s1: hobbled", hurt_id)`. This block reads `id`, so it's alone. If `id` turns out unusable on device, the fix is one name change, in two places.
- [ ] **Step 2: Check it.** Expected new handlers: `presnap, incomplete, touchdown, play_end, sack, play_end x5, snap`.
- [ ] **Step 3: Commit:** `git commit -am "Step 1 scripts: light injuries with identity probe"`

### Task 5: Whole-file checks and docs

**Files:**
- Modify: `examples/step1_script.txt` (only if a check fails)
- Modify: `docs/SCRIPTING.md` (one line under "Example": the step 1 script and what it adds)

- [ ] **Step 1: Check the file alone.** It loads, and the limits check prints `blocks <= 48`, `vars <= 64`.
- [ ] **Step 2: Check it together with the Hail Mary.** Concatenate `examples/rbmod_script.txt` and `examples/step1_script.txt` and run both checks. It loads, `blocks <= 48`, `vars <= 64`.
- [ ] **Step 3: Read every `on frame` block** for a variable another section also sets (Review Focus 3), and every block's first condition for its `*_on` switch (Review Focus 2). Fix anything found.
- [ ] **Step 4: Add the SCRIPTING.md line, then commit:** `git commit -am "Step 1 scripts: checked with the Hail Mary; docs pointer"`
