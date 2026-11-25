# gridlock-stl-inspector

Out-of-tree **C++23 native plugin** for the **GridLock** MPI debugger (`C99 ABI v1.0`).

`gridlock-stl-inspector` inspects contiguous numeric C++ standard library containers (`std::vector`, `std::array`, `std::valarray`, and `std::span` over `float`, `double`, and 32/64-bit signed/unsigned integers) on stopped MPI ranks and computes non-mutating summary reductions (**Min**, **Max**, **Arithmetic Mean**, and **L2 Norm**) directly from inferior memory without modifying target state or linking Qt6/internal GridLock libraries.

---

## Workstation Features

- **Variables Dock Context Action (`register_variable_action`)**:
  - Registers **"Compute Vector Summary Stats (Min / Max / Mean / L2)"** for variables matching `.*(std::vector|std::array|std::valarray|std::span).*`.
  - Evaluates `<expr>.size()` and `<expr>.data()` (with `&(<expr>[0])` fallback for `std::valarray`) via `gridlock::plugin::evalExpr` and samples up to `1024` numeric elements (`f32`, `f64`, `i32`, `u32`, `i64`, `u64`) via `gridlock::plugin::readBytes`.
- **`Tools -> Plugins` Commands & Nested Submenu (`register_command_action` / `register_menu_action`)**:
  - **"STL Inspector: Summarize Active Rank Stop"**: Recomputes summary statistics for the tracked container on the focused MPI rank (or logs current stop status when idle).
  - **"STL Inspector / Inspect Container Expression..."** (`Ctrl+Alt+Shift+S`): Prompts interactively via `gridlock::plugin::promptInput` for any C++ container expression.
  - **"STL Inspector / Open JSON Telemetry Tree"**: Publishes the latest structured payload into an expandable `GRIDLOCK_REPORT_JSON_TREE` tab.
  - **"STL Inspector / Clear Annotations & Reset"**: Clears tracked container state, inline source badges, and report panels.
- **Multi-Surface Context Menus (`register_context_action`)**:
  - **Source Editor**: Right-click a symbol or highlighted expression -> **"Inspect Selected STL Container (Min / Max / Mean / L2)"**.
  - **Call Stack**: Right-click a stack frame -> **"Summarize Tracked STL Container in Frame"**.
  - **Server Rack / MPI Rank List**: Right-click any MPI rank card -> **"Summarize Tracked STL Container on Rank"**.
- **Status-Bar Pill, Inline Editor Badges, Toasts & Reports**:
  - Mounts a clickable status-bar badge (`STL R0: L2=...`, switching to `"error"` styling on `NaN` / `Inf` detection) that opens a `GRIDLOCK_REPORT_MARKDOWN` summary tab in **Plugin Reports** and jumps to the last stop location.
  - Attaches inline source-line annotations (`set_editor_annotation`) on stopped frames and syncs live workstation palette tokens (`get_theme_info` + `on_theme_changed`).
- **Automatic Stop-Hook Tracking & Structured Telemetry**:
  - Once you inspect a container variable (or set `GRIDLOCK_STL_INSPECTOR_EXPR=<expr>`), `on_rank_stopped` refreshes summary statistics automatically whenever an MPI rank stops.
  - Logs concise one-line summaries via `host->log_message` and emits RFC 8259 JSON payloads on channel `"gridlock.plugin.stl_inspector"`:
    ```json
    {
      "rank": 0,
      "expr": "residual_vec",
      "count": 1024,
      "min": -0.04125,
      "max": 0.9831,
      "mean": 0.00142,
      "l2_norm": 4.819204
    }
    ```
- **Strict ABI Isolation**:
  - Built with `-fvisibility=hidden`, `-fvisibility-inlines-hidden`, and a linker version script exporting **only** `gridlock_plugin_init_v1`.
  - All C ABI callbacks are `noexcept` + `try / catch (...)` guarded with heap-owned `PluginState` freed symmetrically in `shutdown`.

---

## Requirements

- C++23 compiler (`GCC >= 13` or `Clang >= 17`)
- [Meson](https://mesonbuild.com/) (`>= 0.63`) and [Ninja](https://ninja-build.org/)
- GridLock native plugin ABI v1.0 headers (`gridlock/plugin_api.h` and `gridlock/plugin_cpp_helpers.hpp`)

---

## Installation

### Option 1: One-Click Install in GridLock (`Preferences -> Plugins -> Install from Git`)

1. Open **Edit -> Preferences** (`Ctrl+,`), select **Plugins**, and switch to the **Install from Git** tab.
2. Keep **Git Host** set to **GitHub (`github.com`)** and enter the shorthand (or full URL):
   ```text
   bnorthern42/gridlock-stl-inspector
   ```
3. Choose **Global (`~/.config/gridlock/plugins`)** or **This Workspace (`<workspace>/.gridlock/plugins`)** and click **Clone & Build**.
4. Click **Restart GridLock Now** when prompted to activate the plugin.

### Option 2: Declarative `plugins.toml` Manifest

Add an entry to `~/.config/gridlock/plugins.toml` (Global) or `<workspace>/.gridlock/plugins.toml` (Workspace), then click **Sync `plugins.toml`** in **Preferences -> Plugins -> Install from Git**:

```toml
[[plugins]]
repo = "bnorthern42/gridlock-stl-inspector"
build = "auto"
enabled = true
```

### Option 3: Manual Build & Placement

Configure and build with Meson + Ninja (pass `-Dgridlock_include_dir=...` if GridLock's headers are not installed under `/usr/local/include`):

```bash
meson setup builddir -Dgridlock_include_dir=/usr/local/include
ninja -C builddir
```

Verify that `gridlock_plugin_init_v1` is the sole exported dynamic symbol:

```bash
nm -D --defined-only builddir/libgridlock_stl_inspector.so
```

Copy the built shared module into your global or workspace GridLock plugins directory:

```bash
mkdir -p ~/.config/gridlock/plugins
cp builddir/libgridlock_stl_inspector.so ~/.config/gridlock/plugins/
```

*(On macOS, copy `builddir/libgridlock_stl_inspector.dylib`.)*

---

## License

Released under the [MIT License](LICENSE).
