# retrobowl-mods

Gameplay mod library for Retro Bowl 1.6.49 (iOS). The mods are C code compiled into
`libRBMod.dylib`. A patched game binary loads it at launch and jumps into it at hook points.

- `src/rb.h`: game runtime bindings (addresses, value types, helpers)
- `src/hooks.def`: list of hooked game functions
- `src/hooks.c`: the mods
- `tools/inject.py`: patches a game binary (load command + hook stubs)
- `make`: builds the dylib on macOS/Xcode; `make LINUX=1` builds with clang + ld64.lld

Game files are not included in this repo.

## Workshop and play scripts

- `site/`: the Workshop page (Tune, Rules, Script, Report, Files); `python3 tools/build_site.py` builds `site/index.html`
- `src/script.c`, `src/script_game.c`: the play-script engine for `rbmod_script.txt` (mod build v89+)
- `docs/SCRIPTING.md`: the script language: every event, statement and function
- `examples/`: the Hail Mary script and its button rules, ready to drop into the game's folder
- `tools/trace_plot.py`: draws plays traced by the step 1 script (`s1: TS/TB/TP` lines in rbmod.log) as PNG pictures
