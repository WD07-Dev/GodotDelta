# GodotDelta

GodotDelta is a Godot modding and patching tool for `Godot 3.x` and `Godot 4.x` games.

It is built for workflows where you want to:
- build a runtime patch from a Godot project
- distribute either a plain patch `.pck` or a protected `.gdmod`
- apply a patch to a base game
- test changes in a sandbox without touching the original game
- rebuild and retest quickly during development

## What It Supports

- Base game input:
  - standalone `.pck`
  - `.exe` with embedded `.pck`
  - `.exe` with a sibling `.pck` using the same file name stem
- Engine targets:
  - `Godot 3.x`
  - `Godot 4.x`

## Package Formats

### Patch `.pck`

Plain runtime patch format.

- created with `gddelta make`
- applied with `gddelta apply`
- best for testing, debugging, or external loaders such as GodotMods

### `.gdmod`

Protected distribution format.

- created with `gddelta make`
- applied with `gddelta apply`
- can require the original base game to recover the payload
- best when you do not want to ship a plain patch `.pck`

## Main Commands

### Make

```bash
gddelta make <base.pck|base.exe> <project_dir> <output.gdmod|output.pck>
```

Examples:

```bash
gddelta make game.exe my_mod_project mod.gdmod
gddelta make game.exe my_mod_project mod.pck
```

The output extension decides what is created:
- `.gdmod` -> protected package
- `.pck` -> plain patch package

### Apply

```bash
gddelta apply <base.pck|base.exe> <input.pck|input.gdmod> [output]
```

Behavior depends on the input and optional output path:
- no output path:
  - patch the base game in place
- input is `.pck` and output is a directory:
  - build a sandbox there
- input is `.gdmod` and output is a directory:
  - recover the patch and build a sandbox there
- input is `.gdmod` and output is a `.pck` path:
  - recover a plain patch `.pck`

Examples:

```bash
gddelta apply game.exe mod.pck
gddelta apply game.exe mod.gdmod
gddelta apply game.exe mod.pck output/dev-runtime
gddelta apply game.exe mod.gdmod output/dev-runtime
gddelta apply game.exe mod.gdmod recovered_patch.pck
```

### Dev Build

```bash
gddelta dev-build <base.pck|base.exe> <project_dir> <sandbox_dir>
```

Example:

```bash
gddelta dev-build game.exe my_mod_project output/live-dev
```

This builds a runnable sandbox from the base game and the current project state.

### Watch

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

## Other Commands

```bash
gddelta ui
gddelta bootstrap
gddelta inspect <input.pck|input.exe>
gddelta diff <base_dir> <modified_dir>
gddelta patch <base_dir> <modified_dir> <output.pck>
gddelta compose <base.pck|base.exe> <patch.pck|project_dir> <output.pck|output.exe>
```

## `.gddeltainclude`

Project-directory commands require a `.gddeltainclude` file in the project root.

That includes commands such as:
- `make`
- `dev-build`
- `watch-dev-build-patch`
- `watch`

If `.gddeltainclude` is missing, GodotDelta stops with an error instead of scanning the whole project.

### Include Rules

- `path` or `glob`: include
- `+path` or `+glob`: force include
- `!path` or `!glob`: exclude

Both forms are accepted:
- `UI/**`
- `res://UI/**`

### Path Mapping

You can map a project source path to a different pack path with `=`.

Examples:

```text
project.godot=game.godot
res://misc/epilepsy_warning.tscn=res://edit/epilepsy_warning.tscn
"res://misc/epilepsy_warning.tscn"="res://edit/epilepsy_warning.tscn"
```

Meaning:
- left side: path to write inside the patch pack
- right side: actual source file to read from the project

### Example

```text
!res://.autoconverted/**
+res://fonts/ko_mono.ttf
+res://misc/epilepsy_warning.tscn
project.godot=game.godot
```

Packaged builds also read the default include file named:

```text
default.gddeltainclude
```

## Base Encryption Key

Some encrypted games need a base encryption key.

Use:

```bash
--base-key <64-char-hex>
```

Examples:

```bash
gddelta make game.exe my_mod_project mod.gdmod --base-key 0123...
gddelta dev-build game.exe my_mod_project output/live-dev --base-key 0123...
```

The GUI also exposes a `Base Key` field on Dev tabs.

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

Current behavior:
- command execution runs in the background
- logs stream into the window while the command runs
- buttons and inputs are disabled while a command is running
- the `Base Key` field is only shown on Dev tabs

## GDRETools Integration

GodotDelta uses [GDRETools](https://github.com/GDRETools/gdsdecomp) where accurate Godot package and bytecode handling matters most.

Current use cases include:
- Godot `3.x` GDScript bytecode compilation
- legacy `3.x` patch application paths
- pack inspection / recovery paths where engine-specific behavior matters

GDRETools is prepared lazily:
- it is not downloaded at startup
- it is only prepared when a command actually needs it

## Notes

- `apply` without an output path modifies the base game.
- `apply` with a sandbox directory leaves the original base untouched.
- `dev-build` and watch workflows are intended for testing and iteration.
- for non-embedded `.exe` inputs, GodotDelta tries to use a sibling `.pck` with the same stem
- legacy Godot `3.x` patching can behave differently from Godot `4.x` because of bytecode and remap handling

## Credits

- Some Godot pack, patching, and bytecode behavior was studied with reference to [gdsdecomp](https://github.com/GDRETools/gdsdecomp).