# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

orion2re is a C++20 reimplementation of the Master of Orion II engine (`orion95.exe`), running on SDL3 against the user's own original game data (LBX archives from a GOG or Steam copy). Upstream is a restoration project: preserving original game behavior, save compatibility, multiplayer compatibility, determinism, and mod compatibility outranks cleanup or redesign (see CONTRIBUTING.md). Never add material copied or extracted from the original game (assets, text, data) to the repository.

**This fork exists to make the game playable with a screen reader.** The accessibility layer in `src/access/` (described under Architecture below) is the main line of work. Keep its changes to engine code small and contained so the fork can keep merging upstream, and so the hooks could be offered upstream. The layer must leave gameplay, saves, and multiplayer behavior unchanged.

## Building

Vendored dependencies are git submodules; run `git submodule update --init --recursive` first.

Configure and build with presets from `CMakePresets.json` (Ninja generator), e.g.:

```sh
cmake --preset win64-debug
cmake --build --preset win64-debug
```

- Windows presets use `"strategy": "external"`, so run CMake from an MSVC developer environment (`vcvarsall.bat x64` or `x86`) with Ninja on PATH (Visual Studio bundles one under `Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja`).
- `-DORION2RE_GAME_DIR=<path>` makes building `orion2re` copy the exe, pdb, `data/base.zip`, the default `data/orion2re.cfg`, and the bundled mod archives into a game directory. The game must run with that directory as its working directory.
- Tests are opt-in: `-DORION2RE_BUILD_TESTS=ON`. With tests on, `ORION2RE_EXCLUDE_RUNTIME_FROM_ALL` defaults to ON and silently drops `orion2re` from a plain build, so either name the target (`--target orion2re`) or set that option OFF. A gitignored `CMakeUserPresets.json` is the place for local combinations of these.
- Router-only (no SDL game): `cmake --preset linux-router`.

## Tests

Unit tests use doctest. Each test executable is declared in `cmake/tests/unit.cmake` with `orion2re_add_unit_test(<name> SOURCES ... PRODUCTION_SOURCES ... FAKE_SOURCES ...)`: it compiles only the listed production `.cpp` files plus fakes from `tests/fakes/` that stub the rest of the engine. When production code gains a call into another subsystem, every test that compiles that file needs a matching fake.

```sh
tools/run-tests.sh                                   # configure, build, run everything (POSIX shell)
ctest --test-dir <build-dir> -R <test_name>          # one test executable
<build-dir>/<test_name> -tc="<test case name>"       # one doctest case
```

Some existing tests (`tests/unit/mapgen_test.cpp`, `tests/fakes/fake_mapgen_planet_max_runtime.cpp`) currently fail to compile on MSVC (missing `<stdexcept>`), which stops an all-targets build; build specific targets instead.

## Generated code

`src/config/generated/` is produced by `config-generator` from `specs/config/main.json5` (target `config-regenerate`, run automatically when `ORION2RE_BUILD_CONFIG_TOOL` is on). Change the schema or generator, never the generated files. Each gameplay subsystem's tunables live in a `*_config.cpp` beside it.

## Architecture

- **Two threads** (`docs/content/development/threading.md`). The platform thread (`src/game/platform.cpp`) owns SDL: window, events, presentation, and translation of SDL keys into the original DOS-style key codes. The game thread runs the restored `MOX2::main2()` and talks to the platform only through mutex-protected handoffs (input queue, published frames, cursor). The game thread exits by throwing `platform::process_exit_requested` from `platform::Check_Exit()`, so code must never swallow that exception.
- **Screens are blocking loops**, not a state stack. `MOX::current_screen` (the `SCREEN` enum in `orion2_consts.h`) selects the top-level screen; each screen function loops on `fields::Get_Input()` and compares the result with the field indices it registered. Popups and dialogs run nested loops of their own inside a screen.
- **The `fields` module** (`src/game/fields.cpp`, `docs/content/development/fields.md`) is the whole UI and input layer: a global array of hit regions, buttons, toggles, lists, sliders, and text inputs rebuilt by each screen, plus per-screen help lists mapping rectangles to HELP.LBX records. Payloads point into caller-owned memory, and the typed `Field_Data_*` helpers terminate the program on a type mismatch.
- **Assets** load through `vfs` (game directory, `data/base.zip`, and enabled mod packages) and `farload` (LBX entries and data records). Game strings are CP850 with inline layout and substitution commands (`docs/content/authoring-mods/text-formatting.md`).
- **Mods** (`data/mods/<id>/`, `src/config/mod_manager.cpp`) combine CFG overrides, VFS asset replacement, and sandboxed integer-only Lua (`src/lua/`). Lua is deliberately a deterministic simulation API, not a UI scripting system. Mods are activated only by `enable <id>;` in `data/orion2re.cfg`.
- **Networking** (`src/network/`, `docs/content/development/networking.md`) replaces DirectPlay with a TCP router protocol; `orion2re-router` is the standalone router.
- **Debug overlay**: an ImGui inspector and editor enabled with `interface debugger_enabled = true;` and toggled with F10.
- **Screen reader layer** (`src/access/`, `docs/content/development/accessibility.md`): a parallel navigation tree built on the Graph A11y Kernel, with speech through Tolk.
  - `src/access/graph/` is lifted verbatim from another project and keeps its own style (including `std::logic_error` throws); keep it byte-close to upstream.
  - The game hooks are `Offer_Key_Down` in `platform.cpp`, `Tick` and the activation handoff in `fields::Get_Input()`, the shown title and text in `TEXTBOX::Do_Text_Box()`, the shown research areas in `TECH::_Tech_Select()`, the shown question in `GENDRAW::Confirmation_Box()`, `COLONY::awaiting_input` around the colony loop's `Get_Input`, `COLBLDG::awaiting_input` around the build screen loop's `Get_Input`, the turn summary's buttons in `TURNSUM::Turn_Summary_Popup()`, `MAINPUPS::officer_popup_awaiting_input` around the leader-for-hire popup's `Get_Input`, `MAINPUPS::level_popup_awaiting_input` and `MAINPUPS::selection_popup_awaiting_input` around the officer level and planet selection popups' `Get_Input`, the shown discovery in `SCIENCE::Science_Room()`, and lifecycle calls in `MOX2::thunk_main2()`.
  - Each screen is a recipe in `src/access/screens.cpp`, matched against live state every tick. A generic recipe turns any screen's field list into a navigable menu, labelled from field text and HELP.LBX titles. Screens that draw their values as text or pictures (New Game settings, the galaxy map) need dedicated recipes.
  - Activation goes through the game's own hotkey press path rather than reimplementing screen logic, and readouts must not reveal anything a sighted player can't currently see.
  - `speech.log` in the working directory records every spoken line, so it works as a test oracle without a screen reader running.

## Code style

Follow STYLEGUIDE.md. The essentials:

- C-oriented C++20: plain `s_` structs, `e_` enums, `t_` aliases, free functions in subsystem namespaces, no class hierarchies, no exceptions or RTTI-based designs in project code, no operator overloading or hidden global registration.
- Functions are `Pascal_Case_With_Underscores`; file-local helpers end in `_`; variables and members are `lower_case`; file names are `lower_case.cpp`/`.h`.
- Engine `.cpp` files begin with `#include "pch.h"`, which pulls in most game headers.
- Comments explain constraints and non-obvious reasoning, not what the code does.

Commit subjects are short and imperative. Keep behavior changes separate from refactoring and formatting.
