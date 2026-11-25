/**
 * @file plugin.cpp
 * @brief Out-of-tree C++23 GridLock native plugin (ABI v1.0) for inspecting C++ STL containers.
 *
 * Computes non-mutating summary reductions (Min, Max, Arithmetic Mean, and L2 Norm)
 * over contiguous numeric C++ standard containers (`std::vector`, `std::array`,
 * `std::valarray`, `std::span`) across stopped MPI ranks using strictly GridLock's
 * frozen C99 ABI v1.0 (`gridlock/plugin_api.h` — 26 host callbacks, `296 B` table)
 * and header-only C++23 wrappers (`gridlock/plugin_cpp_helpers.hpp`).
 *
 * Workstation Surfaces Integrated:
 * - Variables Dock type-regex action (`register_variable_action`)
 * - Tools -> Plugins flat command (`register_command_action`) + nested submenu (`register_menu_action`)
 * - Interactive container expression prompt (`gridlock::plugin::promptInput`)
 * - Source Editor, Call Stack, and MPI Rank List context actions (`register_context_action`)
 * - Live clickable status-bar badge (`set_status_item`) & non-blocking toasts (`show_toast`)
 * - Inline source-line stop annotations (`set_editor_annotation` / `clear_editor_annotations`)
 * - Declarative Markdown & JSON Tree report tabs (`publish_report_view` / `close_report_view`)
 * - Live workstation theme synchronization (`get_theme_info` + `on_theme_changed`)
 * - Structured JSON telemetry on `"gridlock.plugin.stl_inspector"` (`emit_structured_event`)
 */

#include "gridlock/plugin_api.h"
#include "gridlock/plugin_cpp_helpers.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace gp = gridlock::plugin;

/** @brief Maximum number of numeric elements sampled per container read. */
constexpr std::size_t kMaxSampleElements = 1024u;

/** @brief Structured telemetry channel published by this plugin. */
constexpr const char* kTelemetryChannel = "gridlock.plugin.stl_inspector";

/** @brief Stable plugin descriptor metadata. */
constexpr const char* kPluginId = "io.github.bnorthern42.gridlock_stl_inspector";
constexpr const char* kPluginName = "GridLock STL Container Inspector";
constexpr const char* kPluginVersion = "1.0.0";
constexpr const char* kPluginDescription =
    "Computes non-mutating Min / Max / Mean / L2 summary statistics, inline badges, and reports for contiguous C++ STL containers across MPI ranks.";

/** @brief Primary variable action & top-level command identifiers. */
constexpr const char* kVarActionId = "stl_inspector.compute_vector_summary_stats";
constexpr const char* kVarActionLabel = "Compute Vector Summary Stats (Min / Max / Mean / L2)";
constexpr const char* kVarTypeRegex = ".*(std::vector|std::array|std::valarray|std::span).*";

constexpr const char* kCmdActionId = "stl_inspector.summarize_active_rank_stop";
constexpr const char* kCmdActionLabel = "STL Inspector: Summarize Active Rank Stop";

/** @brief Nested `Tools -> Plugins -> STL Inspector` menu action identifiers. */
constexpr const char* kMenuInspectExprId = "stl_inspector.menu.inspect_expression";
constexpr const char* kMenuInspectExprPath = "STL Inspector/Inspect Container Expression...";
constexpr const char* kMenuOpenJsonReportId = "stl_inspector.menu.open_json_report";
constexpr const char* kMenuOpenJsonReportPath = "STL Inspector/Open JSON Telemetry Tree";
constexpr const char* kMenuClearResetId = "stl_inspector.menu.clear_and_reset";
constexpr const char* kMenuClearResetPath = "STL Inspector/Clear Annotations & Reset";

/** @brief Multi-surface context-menu action identifiers. */
constexpr const char* kCtxEditorInspectId = "stl_inspector.ctx.editor_inspect";
constexpr const char* kCtxEditorInspectLabel = "Inspect Selected STL Container (Min / Max / Mean / L2)";
constexpr const char* kCtxEditorFilterRegex = "^[A-Za-z_*&(][A-Za-z0-9_.:>\\[\\]()\\s*&-]{0,240}$";

constexpr const char* kCtxCallStackId = "stl_inspector.ctx.callstack_summarize";
constexpr const char* kCtxCallStackLabel = "Summarize Tracked STL Container in Frame";

constexpr const char* kCtxRankListId = "stl_inspector.ctx.rank_summarize";
constexpr const char* kCtxRankListLabel = "Summarize Tracked STL Container on Rank";

/** @brief Status bar, annotation, and report panel identifiers. */
constexpr const char* kStatusItemId = "stl_inspector.status";
constexpr const char* kAnnotationId = "stl_inspector.stop_summary";
constexpr const char* kMarkdownPanelId = "stl_inspector.summary_md";
constexpr const char* kMarkdownPanelTitle = "STL Inspector";
constexpr const char* kJsonPanelId = "stl_inspector.telemetry_json";
constexpr const char* kJsonPanelTitle = "STL Inspector (JSON)";

enum class NumericKind : uint8_t {
  Float32,
  Float64,
  Int32,
  UInt32,
  Int64,
  UInt64,
};

[[nodiscard]] constexpr const char* numericKindLabel(NumericKind kind) noexcept {
  switch (kind) {
    case NumericKind::Float32:
      return "f32";
    case NumericKind::Float64:
      return "f64";
    case NumericKind::Int32:
      return "i32";
    case NumericKind::UInt32:
      return "u32";
    case NumericKind::Int64:
      return "i64";
    case NumericKind::UInt64:
      return "u64";
  }
  return "f64";
}

struct SummaryStats {
  std::size_t sampled_count = 0u;
  uint64_t total_count = 0u;
  double min_val = 0.0;
  double max_val = 0.0;
  double mean_val = 0.0;
  double l2_norm = 0.0;
  NumericKind kind = NumericKind::Float64;
  bool has_non_finite = false;
};

/**
 * @struct PluginState
 * @brief Heap-allocated per-instance plugin state stored in `out_desc->plugin_ctx`.
 */
struct PluginState {
  std::string tracked_expr;
  std::string tracked_type_hint;
  std::string last_stop_reason;
  std::string last_stop_file;
  std::string last_stop_function;
  std::string last_json_payload;
  std::string theme_id = "gridlock_slate";
  std::string accent_hex = "#38bdf8";
  std::string warning_hex = "#f59e0b";
  std::string error_hex = "#ef4444";
  SummaryStats last_stats{};
  int32_t last_inspected_rank = -1;
  int32_t last_stopped_rank = -1;
  int32_t last_stop_line = -1;
  int32_t session_rank_count = 0;
  uint64_t stop_events_seen = 0u;
  bool theme_is_dark = true;
  bool has_last_stats = false;
};

enum class TriggerMode : uint8_t {
  InteractiveUser,
  AutomaticStopHook,
};

[[nodiscard]] std::string_view trimAscii(std::string_view raw) noexcept {
  while (!raw.empty() &&
         (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\n' ||
          raw.front() == '\r' || raw.front() == '"')) {
    raw.remove_prefix(1);
  }
  while (!raw.empty() &&
         (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\n' ||
          raw.back() == '\r' || raw.back() == '"')) {
    raw.remove_suffix(1);
  }
  return raw;
}

/**
 * @brief Sanitizes single-line UI strings (`badge`, `text`, `title`) to ensure zero control chars.
 */
[[nodiscard]] std::string sanitizeSingleLine(std::string_view raw, std::size_t maxBytes = 200u) {
  std::string out;
  const std::size_t limit = (raw.size() > maxBytes) ? maxBytes : raw.size();
  out.reserve(limit);
  for (std::size_t i = 0u; i < limit; ++i) {
    const auto uch = static_cast<unsigned char>(raw[i]);
    if (uch >= 0x20u && uch != 0x7Fu) {
      out.push_back(raw[i]);
    } else {
      out.push_back(' ');
    }
  }
  return out;
}

[[nodiscard]] bool needsParentheses(std::string_view expr) noexcept {
  for (const char ch : expr) {
    const bool isIdentOrMember =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ':' ||
        ch == '[' || ch == ']';
    if (!isIdentOrMember) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Parses a decimal or hexadecimal unsigned 64-bit integer from debugger output.
 *
 * Handles plain integers (`"256"`), hex literals (`"0x7ffd1234"`), and typed GDB/LLDB
 * pointer summaries (`"(const double *) 0x55555556aeb0"`).
 */
[[nodiscard]] bool parseUnsigned64(std::string_view raw, uint64_t* outValue) noexcept {
  if (outValue == nullptr) {
    return false;
  }
  std::string_view view = trimAscii(raw);
  if (view.empty()) {
    return false;
  }

  int base = 10;
  if (const std::size_t hexLower = view.find("0x"); hexLower != std::string_view::npos) {
    view.remove_prefix(hexLower + 2u);
    base = 16;
  } else if (const std::size_t hexUpper = view.find("0X"); hexUpper != std::string_view::npos) {
    view.remove_prefix(hexUpper + 2u);
    base = 16;
  } else {
    while (!view.empty() && (view.front() < '0' || view.front() > '9')) {
      if (view.front() == '-') {
        return false;
      }
      view.remove_prefix(1);
    }
  }

  if (view.empty()) {
    return false;
  }

  uint64_t parsed = 0u;
  const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), parsed, base);
  (void)ptr;
  if (ec != std::errc{}) {
    return false;
  }
  *outValue = parsed;
  return true;
}

[[nodiscard]] bool containsSubstr(std::string_view haystack, std::string_view needle) noexcept {
  return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] NumericKind classifyTypeString(
    std::string_view typeText,
    bool* outMatched) noexcept {
  *outMatched = true;
  if (containsSubstr(typeText, "double")) {
    return NumericKind::Float64;
  }
  if (containsSubstr(typeText, "float")) {
    return NumericKind::Float32;
  }
  if (containsSubstr(typeText, "uint64") || containsSubstr(typeText, "size_t") ||
      containsSubstr(typeText, "unsigned long")) {
    return NumericKind::UInt64;
  }
  if (containsSubstr(typeText, "int64") || containsSubstr(typeText, "ptrdiff_t") ||
      containsSubstr(typeText, "ssize_t") || containsSubstr(typeText, "long")) {
    return NumericKind::Int64;
  }
  if (containsSubstr(typeText, "uint32") || containsSubstr(typeText, "unsigned")) {
    return NumericKind::UInt32;
  }
  if (containsSubstr(typeText, "int32") || containsSubstr(typeText, "int")) {
    return NumericKind::Int32;
  }
  *outMatched = false;
  return NumericKind::Float64;
}

[[nodiscard]] NumericKind detectNumericKind(
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view expr,
    std::string_view typeHint,
    std::string_view dataEvalOutput) noexcept {
  bool matched = false;
  if (!typeHint.empty()) {
    const NumericKind fromHint = classifyTypeString(typeHint, &matched);
    if (matched) {
      return fromHint;
    }
  }
  if (!dataEvalOutput.empty()) {
    const NumericKind fromData = classifyTypeString(dataEvalOutput, &matched);
    if (matched) {
      return fromData;
    }
  }

  const std::string sizeofExpr =
      needsParentheses(expr)
          ? ("sizeof(*((" + std::string(expr) + ").data()))")
          : ("sizeof(*(" + std::string(expr) + ".data()))");
  if (const auto elemBytesRes = gp::evalExpr(host, rank, sizeofExpr); elemBytesRes.has_value()) {
    uint64_t elemSize = 0u;
    if (parseUnsigned64(*elemBytesRes, &elemSize) && elemSize == sizeof(float)) {
      return NumericKind::Float32;
    }
  }
  return NumericKind::Float64;
}

[[nodiscard]] std::string escapeJson(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8u);
  for (const unsigned char uch : input) {
    const char ch = static_cast<char>(uch);
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (uch < 0x20u) {
          char hexBuf[7]{};
          std::snprintf(hexBuf, sizeof(hexBuf), "\\u%04x", static_cast<unsigned int>(uch));
          out.append(hexBuf);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

[[nodiscard]] std::string formatJsonNumber(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  const double normalized = (value == 0.0) ? 0.0 : value;
  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%.12g", normalized);
  return std::string(buf);
}

template <typename Elem>
[[nodiscard]] SummaryStats reduceSpan(
    std::span<const Elem> values,
    uint64_t totalCount,
    NumericKind kind) noexcept {
  SummaryStats stats{};
  stats.sampled_count = values.size();
  stats.total_count = totalCount;
  stats.kind = kind;

  if (values.empty()) {
    return stats;
  }

  double minVal = static_cast<double>(values.front());
  double maxVal = static_cast<double>(values.front());
  double sum = 0.0;
  double scale = 0.0;
  double ssq = 1.0;
  bool sawNaN = false;
  bool sawInf = false;

  for (const Elem raw : values) {
    const double val = static_cast<double>(raw);
    sum += val;
    if constexpr (std::is_floating_point_v<Elem>) {
      if (std::isnan(val)) {
        sawNaN = true;
        continue;
      }
      if (std::isinf(val)) {
        if (val < minVal || std::isnan(minVal)) {
          minVal = val;
        }
        if (val > maxVal || std::isnan(maxVal)) {
          maxVal = val;
        }
        sawInf = true;
        continue;
      }
    }
    if (val < minVal || std::isnan(minVal)) {
      minVal = val;
    }
    if (val > maxVal || std::isnan(maxVal)) {
      maxVal = val;
    }
    const double absVal = std::abs(val);
    if (absVal > 0.0) {
      if (scale < absVal) {
        const double ratio = scale / absVal;
        ssq = 1.0 + ssq * ratio * ratio;
        scale = absVal;
      } else {
        const double ratio = absVal / scale;
        ssq += ratio * ratio;
      }
    }
  }

  stats.has_non_finite = sawNaN || sawInf;
  if (sawNaN) {
    const double quietNan = std::numeric_limits<double>::quiet_NaN();
    stats.min_val = quietNan;
    stats.max_val = quietNan;
    stats.mean_val = quietNan;
    stats.l2_norm = quietNan;
  } else {
    stats.min_val = minVal;
    stats.max_val = maxVal;
    stats.mean_val = sum / static_cast<double>(values.size());
    stats.l2_norm = sawInf ? std::numeric_limits<double>::infinity() : (scale * std::sqrt(ssq));
  }
  return stats;
}

template <typename Elem>
[[nodiscard]] std::expected<SummaryStats, GridLockStatus> readAndReduceTyped(
    const GridLockHostApi* host,
    int32_t rank,
    uint64_t dataAddr,
    std::size_t targetCount,
    uint64_t totalCount,
    NumericKind kind) {
  std::vector<Elem> buffer(targetCount, Elem{0});
  const auto readRes =
      gp::readBytes(host, rank, dataAddr, std::as_writable_bytes(std::span<Elem>(buffer)));
  if (!readRes.has_value()) {
    return std::unexpected(readRes.error());
  }
  const std::size_t elemsRead = (*readRes) / sizeof(Elem);
  if (elemsRead == 0u) {
    return std::unexpected(GRIDLOCK_ERR_MEMORY_READ_FAILED);
  }
  return reduceSpan<Elem>(std::span<const Elem>(buffer.data(), elemsRead), totalCount, kind);
}

[[nodiscard]] std::expected<std::string, GridLockStatus> evalContainerSize(
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view expr) noexcept {
  const std::string base(expr);
  if (!needsParentheses(expr)) {
    auto direct = gp::evalExpr(host, rank, base + ".size()");
    if (direct.has_value()) {
      return direct;
    }
  }
  return gp::evalExpr(host, rank, "(" + base + ").size()");
}

[[nodiscard]] std::expected<std::string, GridLockStatus> evalContainerData(
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view expr,
    uint64_t elementCount) noexcept {
  const std::string base(expr);
  if (!needsParentheses(expr)) {
    auto direct = gp::evalExpr(host, rank, base + ".data()");
    if (direct.has_value()) {
      return direct;
    }
  }
  auto parenthesized = gp::evalExpr(host, rank, "(" + base + ").data()");
  if (parenthesized.has_value()) {
    return parenthesized;
  }
  if (elementCount > 0u) {
    // Fallback for std::valarray<T>, which lacks .data() in ISO C++ but guarantees contiguous &v[0].
    return gp::evalExpr(host, rank, "&((" + base + ")[0])");
  }
  return parenthesized;
}

[[nodiscard]] std::string buildMarkdownReport(
    const PluginState& state,
    int32_t rank,
    std::string_view expr,
    const SummaryStats& stats) {
  char numBuf[256]{};
  std::snprintf(
      numBuf,
      sizeof(numBuf),
      "| **Min** | `%.9g` |\n"
      "| **Max** | `%.9g` |\n"
      "| **Arithmetic Mean** | `%.9g` |\n"
      "| **L2 Norm** | `%.9g` |\n",
      stats.min_val,
      stats.max_val,
      stats.mean_val,
      stats.l2_norm);

  std::string md;
  md.reserve(768u);
  md.append("### STL Container Summary\n\n");
  if (stats.has_non_finite) {
    md.append("> **Warning**: Non-finite (`NaN` / `Inf`) elements detected in sampled buffer.\n\n");
  }
  md.append("| Metric | Value |\n| :--- | :--- |\n");
  md.append("| **MPI Rank** | `").append(std::to_string(rank)).append("` |\n");
  md.append("| **Expression** | `").append(sanitizeSingleLine(expr, 120u)).append("` |\n");
  md.append("| **Element Type** | `").append(numericKindLabel(stats.kind)).append("` |\n");
  md.append("| **Sampled / Total** | `")
      .append(std::to_string(stats.sampled_count))
      .append(" / ")
      .append(std::to_string(stats.total_count))
      .append("` |\n");
  md.append(numBuf);
  if (!state.last_stop_function.empty() || state.last_stop_line > 0) {
    md.append("| **Last Stop** | `")
        .append(state.last_stop_function.empty() ? "<unknown>" : sanitizeSingleLine(state.last_stop_function, 80u))
        .append("` (line `")
        .append(std::to_string(state.last_stop_line))
        .append("`) |\n");
  }
  md.append("| **Theme** | `")
      .append(sanitizeSingleLine(state.theme_id, 64u))
      .append(state.theme_is_dark ? " (dark)` |\n" : " (light)` |\n");
  return md;
}

/* Forward declaration for status-item click callback. */
GridLockStatus GRIDLOCK_CALL onStatusItemClicked(
    void* pluginCtx,
    const GridLockHostApi* host,
    const char* itemIdUtf8) noexcept;

void applyThemeSnapshot(PluginState* state, const GridLockThemeInfo* theme) noexcept {
  if (state == nullptr || theme == nullptr) {
    return;
  }
  if (theme->struct_size != 0u && theme->struct_size < sizeof(GridLockThemeInfo)) {
    return;
  }
  state->theme_is_dark = (theme->is_dark != 0);
  if (theme->theme_id_utf8[0] != '\0') {
    const std::size_t n = ::strnlen(theme->theme_id_utf8, sizeof(theme->theme_id_utf8));
    state->theme_id.assign(theme->theme_id_utf8, n);
  }
  if (theme->accent_hex[0] != '\0') {
    const std::size_t n = ::strnlen(theme->accent_hex, sizeof(theme->accent_hex));
    state->accent_hex.assign(theme->accent_hex, n);
  }
  if (theme->warning_hex[0] != '\0') {
    const std::size_t n = ::strnlen(theme->warning_hex, sizeof(theme->warning_hex));
    state->warning_hex.assign(theme->warning_hex, n);
  }
  if (theme->error_hex[0] != '\0') {
    const std::size_t n = ::strnlen(theme->error_hex, sizeof(theme->error_hex));
    state->error_hex.assign(theme->error_hex, n);
  }
}

void publishSummary(
    PluginState* state,
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view expr,
    const SummaryStats& stats,
    TriggerMode mode) {
  const char* elemLabel = numericKindLabel(stats.kind);

  char logBuf[512]{};
  std::snprintf(
      logBuf,
      sizeof(logBuf),
      "[stl-inspector] rank=%d expr=%.*s count=%zu/%llu (%s) min=%.6g max=%.6g mean=%.6g l2=%.6g",
      rank,
      static_cast<int>(expr.size() > 160u ? 160u : expr.size()),
      expr.data(),
      stats.sampled_count,
      static_cast<unsigned long long>(stats.total_count),
      elemLabel,
      stats.min_val,
      stats.max_val,
      stats.mean_val,
      stats.l2_norm);

  if (host->log_message != nullptr) {
    (void)host->log_message(
        host->host_ctx,
        stats.has_non_finite ? GRIDLOCK_LOG_WARN : GRIDLOCK_LOG_INFO,
        logBuf);
  }

  const std::string jsonPayload =
      std::string("{\"rank\":") + std::to_string(rank) +
      ",\"expr\":\"" + escapeJson(expr) + "\"" +
      ",\"count\":" + std::to_string(stats.sampled_count) +
      ",\"min\":" + formatJsonNumber(stats.min_val) +
      ",\"max\":" + formatJsonNumber(stats.max_val) +
      ",\"mean\":" + formatJsonNumber(stats.mean_val) +
      ",\"l2_norm\":" + formatJsonNumber(stats.l2_norm) + "}";

  if (host->emit_structured_event != nullptr) {
    (void)host->emit_structured_event(host->host_ctx, kTelemetryChannel, jsonPayload.c_str());
  }

  if (state == nullptr) {
    return;
  }

  state->last_stats = stats;
  state->last_inspected_rank = rank;
  state->last_json_payload = jsonPayload;
  state->has_last_stats = true;

  const char* colorToken = stats.has_non_finite ? "error" : "accent";
  const std::string shortExpr = sanitizeSingleLine(expr, 32u);

  if (host->set_status_item != nullptr) {
    char badgeBuf[128]{};
    if (stats.has_non_finite) {
      std::snprintf(badgeBuf, sizeof(badgeBuf), "STL R%d: NaN/Inf!", rank);
    } else {
      std::snprintf(badgeBuf, sizeof(badgeBuf), "STL R%d: L2=%.4g", rank, stats.l2_norm);
    }
    char tipBuf[512]{};
    std::snprintf(
        tipBuf,
        sizeof(tipBuf),
        "Container: %s (rank %d, %zu/%llu %s)\nMin: %.6g | Max: %.6g\nMean: %.6g | L2: %.6g\nClick to open STL Inspector report",
        shortExpr.c_str(),
        rank,
        stats.sampled_count,
        static_cast<unsigned long long>(stats.total_count),
        elemLabel,
        stats.min_val,
        stats.max_val,
        stats.mean_val,
        stats.l2_norm);
    (void)host->set_status_item(
        host->host_ctx,
        kStatusItemId,
        badgeBuf,
        tipBuf,
        colorToken,
        &onStatusItemClicked,
        state);
  }

  if (host->set_editor_annotation != nullptr && !state->last_stop_file.empty() && state->last_stop_line > 0) {
    const std::string cleanFile = sanitizeSingleLine(state->last_stop_file, 4000u);
    char annBadge[128]{};
    if (stats.has_non_finite) {
      std::snprintf(annBadge, sizeof(annBadge), "STL %s: NaN/Inf", shortExpr.c_str());
    } else {
      std::snprintf(annBadge, sizeof(annBadge), "STL %s: L2=%.4g", shortExpr.c_str(), stats.l2_norm);
    }
    (void)host->set_editor_annotation(
        host->host_ctx,
        kAnnotationId,
        cleanFile.c_str(),
        state->last_stop_line,
        annBadge,
        logBuf,
        colorToken);
  }

  if (host->publish_report_view != nullptr && mode == TriggerMode::InteractiveUser) {
    const std::string md = buildMarkdownReport(*state, rank, expr, stats);
    (void)host->publish_report_view(
        host->host_ctx,
        kMarkdownPanelId,
        kMarkdownPanelTitle,
        GRIDLOCK_REPORT_MARKDOWN,
        md.c_str());
  }

  if (host->show_toast != nullptr && (mode == TriggerMode::InteractiveUser || stats.has_non_finite)) {
    char toastBody[256]{};
    std::snprintf(
        toastBody,
        sizeof(toastBody),
        "Rank %d | %s (%zu elems): min=%.4g, max=%.4g, mean=%.4g, L2=%.4g",
        rank,
        shortExpr.c_str(),
        stats.sampled_count,
        stats.min_val,
        stats.max_val,
        stats.mean_val,
        stats.l2_norm);
    (void)host->show_toast(
        host->host_ctx,
        stats.has_non_finite ? GRIDLOCK_DIALOG_ERROR : GRIDLOCK_DIALOG_INFO,
        stats.has_non_finite ? "STL Inspector: Non-Finite Values" : "STL Container Summary",
        toastBody,
        3000);
  }
}

[[nodiscard]] GridLockStatus inspectContainerAndPublish(
    PluginState* state,
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view rawExpr,
    std::string_view typeHint,
    TriggerMode mode) {
  const std::string_view expr = trimAscii(rawExpr);
  if (host == nullptr || expr.empty()) {
    return GRIDLOCK_ERR_INVALID_ARG;
  }
  if (rank < 0) {
    return GRIDLOCK_ERR_INVALID_RANK;
  }

  const auto sizeStrRes = evalContainerSize(host, rank, expr);
  if (!sizeStrRes.has_value()) {
    return sizeStrRes.error();
  }

  uint64_t totalCount = 0u;
  if (!parseUnsigned64(*sizeStrRes, &totalCount)) {
    return GRIDLOCK_ERR_EVAL_FAILED;
  }

  const auto dataStrRes = evalContainerData(host, rank, expr, totalCount);
  if (!dataStrRes.has_value() && totalCount > 0u) {
    return dataStrRes.error();
  }

  uint64_t dataAddr = 0u;
  const std::string_view dataView =
      dataStrRes.has_value() ? std::string_view(*dataStrRes) : std::string_view{};
  if (totalCount > 0u) {
    if (!parseUnsigned64(dataView, &dataAddr) || dataAddr == 0u) {
      return GRIDLOCK_ERR_MEMORY_READ_FAILED;
    }
  }

  const NumericKind kind = detectNumericKind(host, rank, expr, typeHint, dataView);
  const std::size_t targetCount =
      (totalCount > kMaxSampleElements) ? kMaxSampleElements : static_cast<std::size_t>(totalCount);

  SummaryStats stats{};
  stats.total_count = totalCount;
  stats.kind = kind;

  if (targetCount > 0u) {
    std::expected<SummaryStats, GridLockStatus> reduced = std::unexpected(GRIDLOCK_ERR_UNSUPPORTED);
    switch (kind) {
      case NumericKind::Float32:
        reduced = readAndReduceTyped<float>(host, rank, dataAddr, targetCount, totalCount, kind);
        break;
      case NumericKind::Float64:
        reduced = readAndReduceTyped<double>(host, rank, dataAddr, targetCount, totalCount, kind);
        break;
      case NumericKind::Int32:
        reduced = readAndReduceTyped<int32_t>(host, rank, dataAddr, targetCount, totalCount, kind);
        break;
      case NumericKind::UInt32:
        reduced = readAndReduceTyped<uint32_t>(host, rank, dataAddr, targetCount, totalCount, kind);
        break;
      case NumericKind::Int64:
        reduced = readAndReduceTyped<int64_t>(host, rank, dataAddr, targetCount, totalCount, kind);
        break;
      case NumericKind::UInt64:
        reduced = readAndReduceTyped<uint64_t>(host, rank, dataAddr, targetCount, totalCount, kind);
        break;
    }
    if (!reduced.has_value()) {
      return reduced.error();
    }
    stats = *reduced;
  }

  publishSummary(state, host, rank, expr, stats, mode);
  return GRIDLOCK_OK;
}

[[nodiscard]] int32_t resolveTargetRank(
    const PluginState* state,
    const GridLockHostApi* host,
    const GridLockCommandSpec* cmdSpec) noexcept {
  if (cmdSpec != nullptr && cmdSpec->active_rank >= 0) {
    return cmdSpec->active_rank;
  }
  if (host != nullptr && host->get_active_rank != nullptr) {
    int32_t activeRank = -1;
    if (host->get_active_rank(host->host_ctx, &activeRank) == GRIDLOCK_OK && activeRank >= 0) {
      return activeRank;
    }
  }
  if (state != nullptr && state->last_stopped_rank >= 0) {
    return state->last_stopped_rank;
  }
  if (cmdSpec != nullptr && cmdSpec->rank_count > 0) {
    return 0;
  }
  return -1;
}

GridLockStatus GRIDLOCK_CALL onStatusItemClicked(
    void* pluginCtx,
    const GridLockHostApi* host,
    const char* /*itemIdUtf8*/) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (state->has_last_stats && host->publish_report_view != nullptr) {
      const std::string md = buildMarkdownReport(
          *state, state->last_inspected_rank, state->tracked_expr, state->last_stats);
      (void)host->publish_report_view(
          host->host_ctx,
          kMarkdownPanelId,
          kMarkdownPanelTitle,
          GRIDLOCK_REPORT_MARKDOWN,
          md.c_str());
    }
    if (!state->last_stop_file.empty() && state->last_stop_line > 0 &&
        host->navigate_to_location != nullptr) {
      const std::string cleanFile = sanitizeSingleLine(state->last_stop_file, 4000u);
      (void)host->navigate_to_location(host->host_ctx, cleanFile.c_str(), state->last_stop_line, 1);
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onVariableAction(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockVariableContext* varCtx) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr || varCtx == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (varCtx->struct_size != 0u && varCtx->struct_size < sizeof(GridLockVariableContext)) {
      return GRIDLOCK_ERR_ABI_MISMATCH;
    }

    const char* rawExpr =
        (varCtx->expression_utf8 != nullptr && varCtx->expression_utf8[0] != '\0')
            ? varCtx->expression_utf8
            : varCtx->name_utf8;
    if (rawExpr == nullptr || rawExpr[0] == '\0') {
      return GRIDLOCK_ERR_INVALID_ARG;
    }

    const std::string_view typeHint =
        (varCtx->type_utf8 != nullptr) ? std::string_view(varCtx->type_utf8) : std::string_view{};
    state->tracked_expr.assign(trimAscii(rawExpr));
    state->tracked_type_hint.assign(typeHint);

    return inspectContainerAndPublish(
        state,
        host,
        varCtx->rank,
        state->tracked_expr,
        state->tracked_type_hint,
        TriggerMode::InteractiveUser);
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onContextSurfaceAction(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockSurfaceContext* surfaceCtx) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr || surfaceCtx == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (surfaceCtx->struct_size != 0u && surfaceCtx->struct_size < sizeof(GridLockSurfaceContext)) {
      return GRIDLOCK_ERR_ABI_MISMATCH;
    }

    int32_t rank = surfaceCtx->rank;
    if (rank < 0) {
      rank = resolveTargetRank(state, host, nullptr);
    }
    if (rank < 0) {
      return GRIDLOCK_ERR_INVALID_RANK;
    }

    if (surfaceCtx->surface == GRIDLOCK_SURFACE_CODE_EDITOR) {
      const char* candidate =
          (surfaceCtx->selected_text_utf8 != nullptr && surfaceCtx->selected_text_utf8[0] != '\0')
              ? surfaceCtx->selected_text_utf8
              : surfaceCtx->symbol_utf8;
      if (candidate == nullptr || candidate[0] == '\0') {
        return GRIDLOCK_ERR_INVALID_ARG;
      }
      const std::string_view cleaned = trimAscii(candidate);
      if (cleaned.empty()) {
        return GRIDLOCK_ERR_INVALID_ARG;
      }
      state->tracked_expr.assign(cleaned);
      state->tracked_type_hint.clear();
    } else if (surfaceCtx->surface == GRIDLOCK_SURFACE_CALL_STACK &&
               surfaceCtx->file_utf8 != nullptr && surfaceCtx->line > 0 &&
               host->navigate_to_location != nullptr) {
      const std::string cleanFile = sanitizeSingleLine(surfaceCtx->file_utf8, 4000u);
      (void)host->navigate_to_location(host->host_ctx, cleanFile.c_str(), surfaceCtx->line, 1);
    }

    if (state->tracked_expr.empty()) {
      if (host->show_toast != nullptr) {
        (void)host->show_toast(
            host->host_ctx,
            GRIDLOCK_DIALOG_WARNING,
            "STL Inspector",
            "No STL container tracked yet. Right-click a container variable or use Tools -> Plugins -> STL Inspector.",
            3500);
      }
      return GRIDLOCK_OK;
    }

    return inspectContainerAndPublish(
        state,
        host,
        rank,
        state->tracked_expr,
        state->tracked_type_hint,
        TriggerMode::InteractiveUser);
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onCommandAction(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockCommandSpec* cmdSpec) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr || cmdSpec == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (cmdSpec->struct_size != 0u && cmdSpec->struct_size < sizeof(GridLockCommandSpec)) {
      return GRIDLOCK_ERR_ABI_MISMATCH;
    }

    const int32_t rank = resolveTargetRank(state, host, cmdSpec);
    if (rank < 0) {
      return GRIDLOCK_ERR_INVALID_RANK;
    }

    if (cmdSpec->args_utf8 != nullptr) {
      const std::string_view trimmedArgs = trimAscii(cmdSpec->args_utf8);
      if (!trimmedArgs.empty()) {
        state->tracked_expr.assign(trimmedArgs);
        state->tracked_type_hint.clear();
      }
    }

    if (!state->tracked_expr.empty()) {
      return inspectContainerAndPublish(
          state,
          host,
          rank,
          state->tracked_expr,
          state->tracked_type_hint,
          TriggerMode::InteractiveUser);
    }

    // No container tracked yet: report stop status cleanly on both log and telemetry channels.
    if (host->log_message != nullptr) {
      char logBuf[384]{};
      const char* fn =
          state->last_stop_function.empty() ? "<unknown>" : state->last_stop_function.c_str();
      std::snprintf(
          logBuf,
          sizeof(logBuf),
          "[stl-inspector] rank=%d stops=%llu fn=%s line=%d (select a std::vector/array/valarray/span variable to track)",
          rank,
          static_cast<unsigned long long>(state->stop_events_seen),
          fn,
          state->last_stop_line);
      (void)host->log_message(host->host_ctx, GRIDLOCK_LOG_INFO, logBuf);
    }
    if (host->emit_structured_event != nullptr) {
      const std::string jsonPayload =
          std::string("{\"rank\":") + std::to_string(rank) +
          ",\"expr\":\"\",\"count\":0,\"min\":0,\"max\":0,\"mean\":0,\"l2_norm\":0}";
      state->last_json_payload = jsonPayload;
      (void)host->emit_structured_event(host->host_ctx, kTelemetryChannel, jsonPayload.c_str());
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onMenuInspectExpression(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockCommandSpec* cmdSpec) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr || cmdSpec == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (cmdSpec->struct_size != 0u && cmdSpec->struct_size < sizeof(GridLockCommandSpec)) {
      return GRIDLOCK_ERR_ABI_MISMATCH;
    }

    const int32_t rank = resolveTargetRank(state, host, cmdSpec);
    if (rank < 0) {
      return GRIDLOCK_ERR_INVALID_RANK;
    }

    const auto inputRes = gp::promptInput(
        host,
        "STL Container Inspector",
        "Enter C++ contiguous container expression (std::vector / array / valarray / span):",
        state->tracked_expr);
    if (!inputRes.has_value()) {
      if (inputRes.error() == GRIDLOCK_ERR_CANCELLED) {
        return GRIDLOCK_OK;
      }
      return inputRes.error();
    }

    const std::string_view expr = trimAscii(*inputRes);
    if (expr.empty()) {
      return GRIDLOCK_OK;
    }

    state->tracked_expr.assign(expr);
    state->tracked_type_hint.clear();
    const GridLockStatus st = inspectContainerAndPublish(
        state, host, rank, state->tracked_expr, state->tracked_type_hint, TriggerMode::InteractiveUser);
    if (st != GRIDLOCK_OK && host->show_toast != nullptr) {
      (void)host->show_toast(
          host->host_ctx,
          GRIDLOCK_DIALOG_WARNING,
          "STL Inspector: Evaluation Failed",
          "Could not evaluate .size() / .data() for the entered expression on the stopped rank.",
          4000);
    }
    return st;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onMenuOpenJsonReport(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockCommandSpec* cmdSpec) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr || cmdSpec == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (host->publish_report_view == nullptr) {
      return GRIDLOCK_ERR_UNSUPPORTED;
    }

    const std::string jsonDoc =
        state->last_json_payload.empty()
            ? std::string("{\"status\":\"idle\",\"channel\":\"gridlock.plugin.stl_inspector\",\"stops_seen\":") +
                  std::to_string(state->stop_events_seen) + "}"
            : state->last_json_payload;

    return host->publish_report_view(
        host->host_ctx,
        kJsonPanelId,
        kJsonPanelTitle,
        GRIDLOCK_REPORT_JSON_TREE,
        jsonDoc.c_str());
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onMenuClearAndReset(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockCommandSpec* /*cmdSpec*/) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }

    state->tracked_expr.clear();
    state->tracked_type_hint.clear();
    state->last_json_payload.clear();
    state->has_last_stats = false;

    if (host->clear_editor_annotations != nullptr) {
      (void)host->clear_editor_annotations(host->host_ctx, "stl_inspector.");
    }
    if (host->close_report_view != nullptr) {
      (void)host->close_report_view(host->host_ctx, kMarkdownPanelId);
      (void)host->close_report_view(host->host_ctx, kJsonPanelId);
    }
    if (host->set_status_item != nullptr) {
      (void)host->set_status_item(
          host->host_ctx,
          kStatusItemId,
          "STL: Idle",
          "Right-click a C++ STL container variable to track Min / Max / Mean / L2",
          "accent",
          &onStatusItemClicked,
          state);
    }
    if (host->show_toast != nullptr) {
      (void)host->show_toast(
          host->host_ctx,
          GRIDLOCK_DIALOG_INFO,
          "STL Inspector Reset",
          "Cleared tracked container expression, source annotations, and report panels.",
          2500);
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onSessionStart(
    void* pluginCtx,
    const GridLockHostApi* host,
    int32_t rankCount) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    state->session_rank_count = rankCount;
    state->last_stopped_rank = -1;
    state->last_stop_line = -1;
    state->last_stop_reason.clear();
    state->last_stop_file.clear();
    state->last_stop_function.clear();
    state->stop_events_seen = 0u;
    state->has_last_stats = false;

    if (host != nullptr) {
      if (host->clear_editor_annotations != nullptr) {
        (void)host->clear_editor_annotations(host->host_ctx, "stl_inspector.");
      }
      if (host->set_status_item != nullptr) {
        const std::string badge =
            "STL: Ready (" + std::to_string(rankCount > 0 ? rankCount : 0) + "R)";
        (void)host->set_status_item(
            host->host_ctx,
            kStatusItemId,
            badge.c_str(),
            "GridLock STL Container Inspector active for current MPI debug session",
            "accent",
            &onStatusItemClicked,
            state);
      }
      if (host->log_message != nullptr) {
        const std::string msg =
            "[stl-inspector] initialized session across " + std::to_string(rankCount) + " MPI ranks";
        (void)host->log_message(host->host_ctx, GRIDLOCK_LOG_DEBUG, msg.c_str());
      }
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onRankStopped(
    void* pluginCtx,
    const GridLockHostApi* host,
    const GridLockStopInfo* stopInfo) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr || host == nullptr || stopInfo == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    if (stopInfo->struct_size != 0u && stopInfo->struct_size < sizeof(GridLockStopInfo)) {
      return GRIDLOCK_ERR_ABI_MISMATCH;
    }

    state->last_stopped_rank = stopInfo->rank;
    state->last_stop_line = stopInfo->line;
    state->last_stop_reason = (stopInfo->reason_utf8 != nullptr) ? stopInfo->reason_utf8 : "";
    state->last_stop_file = (stopInfo->file_utf8 != nullptr) ? stopInfo->file_utf8 : "";
    state->last_stop_function = (stopInfo->function_utf8 != nullptr) ? stopInfo->function_utf8 : "";
    ++state->stop_events_seen;

    if (!state->tracked_expr.empty()) {
      (void)inspectContainerAndPublish(
          state,
          host,
          stopInfo->rank,
          state->tracked_expr,
          state->tracked_type_hint,
          TriggerMode::AutomaticStopHook);
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onSessionEnd(
    void* pluginCtx,
    const GridLockHostApi* host) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    state->session_rank_count = 0;
    state->last_stopped_rank = -1;
    state->last_stop_line = -1;
    state->last_stop_reason.clear();
    state->last_stop_file.clear();
    state->last_stop_function.clear();
    state->stop_events_seen = 0u;

    if (host != nullptr && host->clear_editor_annotations != nullptr) {
      (void)host->clear_editor_annotations(host->host_ctx, "stl_inspector.");
    }
    if (host != nullptr && host->set_status_item != nullptr) {
      (void)host->set_status_item(
          host->host_ctx,
          kStatusItemId,
          "STL: Idle",
          "Right-click a C++ STL container variable to track Min / Max / Mean / L2",
          "accent",
          &onStatusItemClicked,
          state);
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

void GRIDLOCK_CALL onThemeChanged(
    void* pluginCtx,
    const GridLockThemeInfo* theme) noexcept {
  try {
    applyThemeSnapshot(static_cast<PluginState*>(pluginCtx), theme);
  } catch (...) {
  }
}

void GRIDLOCK_CALL onShutdown(
    void* pluginCtx,
    const GridLockHostApi* /*host*/) noexcept {
  delete static_cast<PluginState*>(pluginCtx);
}

[[nodiscard]] GridLockStatus initStlInspectorPlugin(
    const GridLockHostApi* host,
    GridLockPluginDescriptor* outDesc) noexcept {
  try {
    GridLockStatus status = gp::validateHostAbi(host);
    if (status != GRIDLOCK_OK) {
      return status;
    }
    status = gp::initPluginDescriptorHeader(outDesc);
    if (status != GRIDLOCK_OK) {
      return status;
    }

    auto state = std::make_unique<PluginState>();
    if (const char* envExpr = std::getenv("GRIDLOCK_STL_INSPECTOR_EXPR");
        envExpr != nullptr && envExpr[0] != '\0') {
      state->tracked_expr.assign(trimAscii(envExpr));
    }

    if (const auto themeRes = gp::getThemeInfo(host); themeRes.has_value()) {
      applyThemeSnapshot(state.get(), &(*themeRes));
    }

    if (host->register_variable_action != nullptr) {
      status = host->register_variable_action(
          host->host_ctx,
          kVarActionId,
          kVarActionLabel,
          kVarTypeRegex,
          &onVariableAction,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }
    }

    if (host->register_command_action != nullptr) {
      status = host->register_command_action(
          host->host_ctx,
          kCmdActionId,
          kCmdActionLabel,
          nullptr,
          &onCommandAction,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }
    }

    if (host->register_menu_action != nullptr) {
      status = host->register_menu_action(
          host->host_ctx,
          kMenuInspectExprId,
          kMenuInspectExprPath,
          "Ctrl+Alt+Shift+S",
          &onMenuInspectExpression,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }

      status = host->register_menu_action(
          host->host_ctx,
          kMenuOpenJsonReportId,
          kMenuOpenJsonReportPath,
          nullptr,
          &onMenuOpenJsonReport,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }

      status = host->register_menu_action(
          host->host_ctx,
          kMenuClearResetId,
          kMenuClearResetPath,
          nullptr,
          &onMenuClearAndReset,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }
    }

    if (host->register_context_action != nullptr) {
      status = host->register_context_action(
          host->host_ctx,
          GRIDLOCK_SURFACE_CODE_EDITOR,
          kCtxEditorInspectId,
          kCtxEditorInspectLabel,
          kCtxEditorFilterRegex,
          &onContextSurfaceAction,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }

      status = host->register_context_action(
          host->host_ctx,
          GRIDLOCK_SURFACE_CALL_STACK,
          kCtxCallStackId,
          kCtxCallStackLabel,
          nullptr,
          &onContextSurfaceAction,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }

      status = host->register_context_action(
          host->host_ctx,
          GRIDLOCK_SURFACE_RANK_LIST,
          kCtxRankListId,
          kCtxRankListLabel,
          nullptr,
          &onContextSurfaceAction,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }
    }

    if (host->set_status_item != nullptr) {
      status = host->set_status_item(
          host->host_ctx,
          kStatusItemId,
          "STL: Idle",
          "Right-click a C++ STL container variable to track Min / Max / Mean / L2",
          "accent",
          &onStatusItemClicked,
          state.get());
      if (status != GRIDLOCK_OK) {
        return status;
      }
    }

    outDesc->plugin_id_utf8 = kPluginId;
    outDesc->plugin_name_utf8 = kPluginName;
    outDesc->plugin_version_utf8 = kPluginVersion;
    outDesc->description_utf8 = kPluginDescription;
    outDesc->plugin_ctx = state.release();
    outDesc->on_session_start = &onSessionStart;
    outDesc->on_rank_stopped = &onRankStopped;
    outDesc->on_session_end = &onSessionEnd;
    outDesc->shutdown = &onShutdown;
    outDesc->on_theme_changed = &onThemeChanged;
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

}  // namespace

GRIDLOCK_PLUGIN_EXPORT GridLockStatus GRIDLOCK_CALL gridlock_plugin_init_v1(
    const GridLockHostApi* host,
    GridLockPluginDescriptor* out_desc) {
  try {
    return initStlInspectorPlugin(host, out_desc);
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}
