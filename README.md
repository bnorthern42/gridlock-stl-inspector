# gridlock-stl-inspector

Out-of-tree **C++23 native plugin** for the **GridLock** MPI debugger (`ABI v1.0`).

`gridlock-stl-inspector` inspects contiguous numeric C++ standard library containers (`std::vector`, `std::array`, `std::valarray`, and `std::span`) on stopped MPI ranks and computes non-mutating summary reductions (**Min**, **Max**, **Arithmetic Mean**, and **L2 Norm**) directly from inferior memory without modifying target state or linking Qt6/internal GridLock libraries.

---

## Features

- **Variables Context-Menu Action**:
  - Registers **"Compute Vector Summary Stats (Min / Max / Mean / L2)"** for variables matching `.*(std::vector|std::array|std::valarray|std::span).*`.
  - Evaluates `<expr>.size()` and `<expr>.data()` (with `&(<expr>[0])` fallback for `std::valarray`) via `gridlock::plugin::evalExpr` and samples up to `1024` `float` or `double` elements via `gridlock::plugin::readBytes`.
- **Tools Menu Command**:
  - Registers **"STL Inspector: Summarize Active Rank Stop"** so you can recompute stats for the tracked container when switching active MPI ranks or inspect the latest rank stop state on demand.
- **Automatic Stop-Hook Tracking**:
  - Once you inspect a container variable (or set `GRIDLOCK_STL_INSPECTOR_EXPR=<expr>` in the environment), `on_rank_stopped` automatically refreshes summary statistics whenever an MPI rank hits a breakpoint or steps.
- **Structured Telemetry & Logging**:
  - Logs concise one-line summaries via `host->log_message(..., GRIDLOCK_LOG_INFO, ...)`.
  - Emits RFC 8259 JSON events on channel `"gridlock.plugin.stl_inspector"`:
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
  - Built with `-fvisibility=hidden` and `-fvisibility-inlines-hidden`, exporting **only** `gridlock_plugin_init_v1`.
  - All C ABI callbacks are `noexcept` + `try / catch (...)` guarded with heap-owned `PluginState` cleaned up symmetrically in `shutdown`.

---

## Requirements

- C++23 compiler (`GCC >= 13` or `Clang >= 17`)
- [Meson](https://mesonbuild.com/) (`>= 0.63`) and [Ninja](https://ninja-build.org/)
- GridLock native plugin ABI v1.0 headers (`gridlock/plugin_api.h` and `gridlock/plugin_cpp_helpers.hpp`)

---

## One-Click Installation in GridLock (Recommended)

1. Open GridLock and navigate to **Preferences -> Plugins -> Install from GitHub**.
2. Paste this repository URL:
   ```text
   https://github.com/bnorthern42/gridlock-stl-inspector
   ```
3. Choose **Global** (or **Workspace**) scope and click **Install**.
4. GridLock clones the repository into an isolated staging directory, invokes Meson + Ninja with `-Dgridlock_include_dir=...` automatically, verifies `gridlock_plugin_init_v1`, and loads the compiled plugin immediately.

---

## Manual Build & Installation

Configure and build with Meson + Ninja (pass `-Dgridlock_include_dir=...` if GridLock's headers are not installed under `/usr/local/include`):

```bash
meson setup builddir -Dgridlock_include_dir=/usr/local/include
ninja -C builddir
```

Verify that `gridlock_plugin_init_v1` is the sole exported dynamic text symbol:

```bash
nm -D --defined-only builddir/libgridlock_stl_inspector.so
```

Copy the built shared module into your user GridLock plugins directory:

```bash
mkdir -p ~/.config/gridlock/plugins
cp builddir/libgridlock_stl_inspector.so ~/.config/gridlock/plugins/
```

*(On macOS, copy `builddir/libgridlock_stl_inspector.dylib` into `~/Library/Application Support/gridlock/plugins/`.)*

---

## License

Released under the [MIT License](LICENSE).
