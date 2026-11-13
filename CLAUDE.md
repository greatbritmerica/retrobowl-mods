# Retro Bowl mods

Gameplay mod for Retro Bowl 1.6.49 on iOS (GameMaker). The user plays on their iPhone and doesn't code;
Claude can't run the game, so every change is tested by the user on the phone.

## Working with the user
- **Preview before acting:** say what you'll change and wait for a yes before editing or building.
- Batch fixes: one round of changes per test game, not one prompt per tweak.
- Put every tunable number at the top of its script section with a plain-English comment, so the user
  can tune it in the Workshop without asking.
- History before this repo's local sessions: the claude.ai chat "Retro Bowl iOS mods".

## Two ways to change the game
- **Play scripts** (`examples/*.txt`, language in `docs/SCRIPTING.md`): no build. The game rereads
  `rbmod_script.txt` at the next play. Prefer this whenever the script language can do it.
- **Mod build** (C in `src/`, `README.md`): GitHub Actions builds `libRBMod.dylib` on every push. A new IPA
  only swaps that file inside the user's injected v89 IPA; the game binary doesn't change.

## Script loop
1. Edit `examples/step1_script.txt`. A PreToolUse hook blocks edits to generated files and a PostToolUse
   hook reruns the generators after source edits (`tools/hooks.js`). Another PostToolUse hook runs
   `tools/check_script.js` (the Workshop's own checker plus the 48-block / 128-variable limits (64 before v94), alone and combined with the Hail Mary).
   Node is at `C:/Program Files/nodejs/node.exe` (not on the bash PATH).
2. Run the `mod-pitfall-reviewer` agent on the change, then commit, push (branch `step1-scripts`), then `bash tools/deploy_script.sh` puts the combined file in
   iCloud Drive -> "Retro Bowl mods"; the user moves it into the RetroBowl folder in Files.
3. The user plays, copies `rbmod.log` to the same iCloud folder and says so: run `/playtest`.

## Facts that explain most surprises
- `on catch / incomplete / intercepted / touchdown / play_end` and `landing_x()/landing_y()` come from the
  Extreme defense code: Extreme difficulty only. `play_end` doesn't run after a sack.
- At most 7 pre-snap buttons from rules and scripts (3 before v96); rules add theirs first (the Hail Mary at the end of a half).
- `route()` errors when the route doesn't change, and `route_slant_fake` crashes the game.
- `speed()` multiplies acceleration each call: call it once per play, not every frame.
- No game value tells players apart (`id`, `player_id`, `uid`, `number`, `slot` all read 0); only `qb()`
  and `back()` find the same player again. `ball().holder` reads 0 or -4, never who has it.
- Steering can't pull a lineman out of a block.
- Field: your yard line = (1310 - x) / 20 (20 px a yard; your team attacks toward -x). The game has no
  visible coin toss: possession at MATCH_START is the kicking team. Punts are instant (`s_punt`); the CPU
  punt return turns one into the game's kickoff return (`src/punt.c`; the KICKOFF stage number is learned
  from the log at the first kickoff after app start). v97 adds no hook points: new work rides on `hooks.def`'s.
- The mod already reads second-finger taps while aiming (`offtools.c`, pump fake) and can throw a pass at a
  chosen receiver through the virtual finger (`cpuqb.c`): the basis for tap-to-target throws.
