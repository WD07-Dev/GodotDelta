# GodotDelta

GodotDelta is a Godot-focused patching tool for `Godot 4.x` games.

It is designed for modding and patch workflows where you want to:
- build a small patch PCK from a Godot project
- apply that patch to a base game
- test changes in a sandbox without replacing the original game
- rebuild patches continuously during development

## Why this instead of a generic delta patcher?

Generic delta patchers work on raw binary differences.

GodotDelta works at the Godot package level:
- understands `.pck` and embedded `.exe` inputs
- can auto-detect changed project files for patch creation
- can build runtime-oriented patch PCKs instead of full game rebuilds
- can create a sandbox copy for testing without touching the original game

In practice, this makes it more useful for Godot modding than a normal file delta tool.

## Supported runtime

- Target games: `Godot 4.x`
- Godot `3.x` base packs are currently rejected

## Main workflows

### 1. Make a distribution patch

Build a patch PCK from a base game and a modified Godot project:

```bash
gddelta make-patch <base.pck|base.exe> <project_dir> <output.pck>
```

Example:

```bash
gddelta make-patch game.exe my_mod_project rom_battle_patch.pck
```

This command scans the project scope, detects changed inputs, and writes a patch PCK.

### 2. Apply a patch

Apply a patch directly into the base game:

```bash
gddelta apply <base.pck|base.exe> <patch.pck>
```

Or build a patched sandbox copy instead of overwriting the original:

```bash
gddelta apply <base.pck|base.exe> <patch.pck> <sandbox_dir>
```

Examples:

```bash
gddelta apply game.exe rom_battle_patch.pck
gddelta apply game.exe rom_battle_patch.pck output/dev-runtime
```

Rules:
- `apply` without `sandbox_dir` modifies the base game
- other dev/watch commands are intended to work through sandbox output

### 3. Dev build

Build a runnable sandbox from a base game and project directory:

```bash
gddelta dev-build <base.pck|base.exe> <project_dir> <sandbox_dir>
```

Example:

```bash
gddelta dev-build game.exe my_mod_project output/live-dev
```

### 4. Watch mode

Continuously rebuild a live patch and refresh the sandbox:

```bash
gddelta watch-dev-build-patch <base.pck|base.exe> <project_dir> <patch.pck> <sandbox_dir> [interval_ms]
```

Example:

```bash
gddelta watch-dev-build-patch game.exe my_mod_project output/live_patch.pck output/live-dev 500
```

Meaning:
- `patch.pck`: a live patch cache file that gets rebuilt repeatedly
- `sandbox_dir`: the runnable sandbox copy

## GUI

Packaged builds include:
- `gddelta(.exe)`: CLI
- `GodotDelta(.exe/.x86_64)`: GUI
- `default.gddeltainclude`
- first launch automatically downloads GDRE tools next to `gddelta`

You can start the GUI directly or from CLI:

```bash
gddelta ui
```

## `.gddeltainclude`

`make-patch` uses `.gddeltainclude` from the modder project's root directory to limit project scanning.

Pattern rules:
- normal path or glob: include
- `+path`: force include
- `!path`: exclude

Both forms are accepted:
- `UI/**`
- `res://UI/**`

Example:

```
Room/**
Script/**
+addons/custom_runtime/**
!addons/unused/**
```
## Important notes

- For non-embedded `.exe` inputs, GodotDelta tries to use a sibling `.pck` with the same stem.
- Watch/dev flows are for testing and iteration, not final distribution.
- `apply` is the command that intentionally replaces the original base when no sandbox directory is given.

## Credits

- Some Godot pack and decompilation behavior was studied with reference to [GDRETools](https://github.com/GDRETools/gdsdecomp).
