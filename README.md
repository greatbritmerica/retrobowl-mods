# retrobowl-mods

Gameplay mod library for Retro Bowl 1.6.49 (iOS). The mods are C code compiled into
`libRBMod.dylib`. A patched game binary loads it at launch and jumps into it at hook points.

- `src/rb.h`: game runtime bindings (addresses, value types, helpers)
- `src/hooks.def`: list of hooked game functions
- `src/hooks.c`: the mods
- `tools/inject.py`: patches a game binary (load command + hook stubs)
- `make`: builds the dylib on macOS/Xcode; `make LINUX=1` builds with clang + ld64.lld

Game files are not included in this repo.
