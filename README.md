# GodotDelta

GodotDelta is a Godot modding and patching tool for `Godot 3.x` and `Godot 4.x` games.

It is built for workflows where you want to:
- build a small runtime patch from a Godot project
- distribute either a `.gdmod` package or a patch `.pck`
- apply a package to a base game
- test changes in a sandbox without replacing the original game
- rebuild and retest quickly during development

## What GodotDelta Does

Unlike a generic binary delta patcher, GodotDelta works around Godot package structure and mod workflow:
- reads standalone `.pck` files and embedded game `.exe` files
- auto-detects changed project files inside a scoped project tree
- expands runtime-related dependencies for patch creation
- supports Godot 3.x bytecode patching and Godot 4.x resource patching
- builds sandbox outputs for testing without touching the original game

## Supported Targets

- Base games: `Godot 3.x` and `Godot 4.x`
- Inputs:
  - standalone `.pck`
  - `.exe` with embedded `.pck`
  - `.exe` with a sibling `.pck` using the same file name stem

## Package Formats

### `gdmod`

`gdmod` is GodotDelta's protected distribution format.

- Created with `gddelta make`
- Applied with `gddelta apply`
- Can require the original base game to recover the payload
- Best for distribution when you do not want to ship a plain patch `.pck`

### Patch `.pck`

Patch `.pck` is the plain runtime patch format.

- Created with `gddelta make-pck`
- Applied with `gddelta apply`
- Best for direct testing, debugging, or use with external loaders such as GodotMods

## Main Commands

### Build A `gdmod`

```bash
gddelta make <base.pck|base.exe> <project_dir> <output.gdmod>
```

Example:

```bash
gddelta make game.exe my_mod_project rom_battle.gdmod
```

### Build A Patch PCK

```bash
gddelta make-pck <base.pck|base.exe> <project_dir> <output.pck>
```

Example:

```bash
gddelta make-pck game.exe my_mod_project rom_battle_patch.pck
```

### Apply A Package

Apply directly into the base game:

```bash
gddelta apply <base.pck|base.exe> <input.pck|input.gdmod>
```

Build a patched sandbox instead of overwriting the base:

```bash
gddelta apply <base.pck|base.exe> <input.pck|input.gdmod> <sandbox_dir>
```

Examples:

```bash
gddelta apply game.exe rom_battle_patch.pck
gddelta apply game.exe rom_battle.gdmod
gddelta apply game.exe rom_battle_patch.pck output/dev-runtime
```

### Build A Dev Sandbox

```bash
gddelta dev-build <base.pck|base.exe> <project_dir> <sandbox_dir>
```

Example:

```bash
gddelta dev-build game.exe my_mod_project output/live-dev
```

### Watch And Rebuild During Development

```bash
gddelta watch-dev-build-patch <base.pck|base.exe> <project_dir> <patch.pck> <sandbox_dir> [interval_ms] [--log-file path]
```

Example:

```bash
gddelta watch-dev-build-patch game.exe my_mod_project output/live_patch.pck output/live-dev 500
```

This continuously:
- rebuilds a live patch `.pck`
- refreshes the sandbox output
- keeps a runnable test copy ready

### Other Commands

```bash
gddelta ui
gddelta inspect <input.pck|input.exe>
gddelta diff <base_dir> <modified_dir>
gddelta patch <base_dir> <modified_dir> <output.pck>
gddelta compose <base.pck|base.exe> <patch.pck|project_dir> <output.pck|output.exe>
```

## GUI

Packaged builds include:
- `gddelta(.exe)` for CLI
- `GodotDelta(.exe/.x86_64)` for GUI

You can start the GUI directly or from CLI:

```bash
gddelta ui
```

The GUI supports:
- building `.gdmod`
- building patch `.pck`
- applying either format
- building dev sandboxes
- watch mode

Recent behavior:
- GUI command execution runs in the background instead of blocking the window
- buttons and inputs are disabled while a command is running

## `.gddeltainclude`

GodotDelta uses `.gddeltainclude` in the project root to limit scanning scope.

Pattern rules:
- normal path or glob: include
- `+path`: force include
- `!path`: exclude

Both forms are accepted:
- `UI/**`
- `res://UI/**`

Example:

```text
project.godot
Room/**
Script/**
+addons/custom_runtime/**
!addons/unused/**
```

GodotDelta also reads the packaged default include file at:

```text
build/default.gddeltainclude
```

## GDRETools Integration

GodotDelta uses [GDRETools](https://github.com/GDRETools/gdsdecomp) where accurate Godot package and bytecode handling matters most.

Current use cases include:
- project-file based compose
- Godot 3.x GDScript bytecode compilation
- GDRE-based patch application paths where raw merge logic is not enough

GDRETools is now prepared lazily:
- it is not downloaded at startup
- it is only prepared when a command actually needs it

## Notes

- `apply` without a sandbox path replaces the base game.
- `apply` with a sandbox path leaves the original base untouched.
- `dev-build` and watch workflows are intended for testing and iteration.
- For non-embedded `.exe` inputs, GodotDelta will try to use a sibling `.pck` with the same stem.
- Legacy Godot 3.x patching may use different internal application paths than Godot 4.x because of bytecode handling requirements.

## Credits

- Some Godot pack, patching, and bytecode behavior was studied with reference to [gdsdecomp](https://github.com/GDRETools/gdsdecomp).