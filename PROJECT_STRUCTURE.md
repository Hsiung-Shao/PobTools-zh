# Project Structure

## Technology Stack

| Area | Technology |
|---|---|
| PobTools desktop app | C++17, CMake, Dear ImGui, GLFW, ANGLE/OpenGL ES |
| Path of Building engine | C++, LuaJIT |

## Directory Tree

```text
PobTools/
|-- pob-zh-engine/                 PobTools desktop app and translation engine
|   |-- host/                      Launcher, editors, atlas planner, shared UI theme
|   |   `-- data/                  Prebuilt runtime data (translated dictionaries, trees)
|   |-- engine/                    SimpleGraphic-derived Lua host
|   |-- translate/                 Runtime translation lookup and hooks
|   |-- dep/                       Vendored build dependencies (glm, compressonator, ...)
|   |-- tests/                     Translation integration tests
|   `-- dist/                      Local deployment package (gitignored)
`-- docs/                          Install guide and usage guide
```

## Module Descriptions

- `pob-zh-engine/host/ui_theme.*` owns the shared PobTools palette, spacing, component density, semantic button styles, and headless theme invariants.
- `pob-zh-engine/host/launcher_ui.cpp` renders the game launcher and tool entry points.
- `pob-zh-engine/host/launcher_editor.cpp` renders the translation dictionary editor.
- `pob-zh-engine/host/filter_editor.cpp` and `editor_shell.cpp` own the filter editor application shell.
- `pob-zh-engine/host/atlas_planner.cpp` owns the atlas planner window and its canvas/panel layout.
- `pob-zh-engine/host/data/*.json` holds prebuilt runtime data (translated dictionaries, atlas/passive trees) the app loads at runtime.
- `pob-zh-engine/translate/` owns runtime translation data loading and lookup.

## Documentation

- [README.md](README.md) — overview, download/install for users, build from source.
- [docs/INSTALL.md](docs/INSTALL.md) — end-user install guide.
- [docs/USAGE.md](docs/USAGE.md) — feature-by-feature usage guide.
- [pob-zh-engine/README_PobTools.md](pob-zh-engine/README_PobTools.md) — engine-in-exe architecture.
