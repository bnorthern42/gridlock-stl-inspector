/**
 * @file plugin.cpp
 * @brief Out-of-tree C++23 GridLock native plugin for inspecting C++ STL containers.
 *
 * Computes non-mutating summary statistics (min, max, arithmetic mean, and L2 norm)
 * over contiguous numeric C++ standard containers (`std::vector`, `std::array`,
 * `std::valarray`, `std::span`) on stopped MPI ranks using strictly GridLock's
 * frozen C99 ABI v1.0 (`gridlock/plugin_api.h`) and header-only C++23 helpers
 * (`gridlock/plugin_cpp_helpers.hpp`).
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

/** @brief Stable plugin descriptor and action identifiers. */
constexpr const char* kPluginId = "io.github.bnorthern42.gridlock_stl_inspector";
constexpr const char* kPluginName = "GridLock STL Container Inspector";
constexpr const char* kPluginVersion = "1.0.0";
constexpr const char* kPluginDescription =
    "Computes non-mutating Min / Max / Mean / L2 summary statistics for contiguous C++ STL containers across MPI ranks.";

constexpr const char* kVarActionId = "stl_inspector.compute_vector_summary_stats";
constexpr const char* kVarActionLabel = "Compute Vector Summary Stats (Min / Max / Mean / L2)";
constexpr const char* kVarTypeRegex = ".*(std::vector|std::array|std::valarray|std::span).*";

constexpr const char* kCmdActionId = "stl_inspector.summarize_active_rank_stop";
constexpr const char* kCmdActionLabel = "STL Inspector: Summarize Active Rank Stop";

enum class NumericKind : uint8_t {
  Float32,
  Float64,
};

struct SummaryStats {
  std::size_t sampled_count = 0u;
  uint64_t total_count = 0u;
  double min_val = 0.0;
  double max_val = 0.0;
  double mean_val = 0.0;
  double l2_norm = 0.0;
  NumericKind kind = NumericKind::Float64;
};

/**
 * @struct PluginState
 * @brief Heap-allocated per-instance plugin state stored in `out_desc->plugin_ctx`.
 */
struct PluginState {
  std::string tracked_expr;
  std::string tracked_type_hint;
  std::string last_stop_reason;
  std::string last_stop_function;
  int32_t last_stopped_rank = -1;
  int32_t last_stop_line = -1;
  uint64_t stop_events_seen = 0u;
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
    // Skip any leading type cast prefix such as "(size_t) 128".
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

[[nodiscard]] bool containsWord(std::string_view haystack, std::string_view needle) noexcept {
  return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] NumericKind detectNumericKind(
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view expr,
    std::string_view typeHint,
    std::string_view dataEvalOutput) noexcept {
  if (containsWord(typeHint, "double")) {
    return NumericKind::Float64;
  }
  if (containsWord(typeHint, "float")) {
    return NumericKind::Float32;
  }
  if (containsWord(dataEvalOutput, "double")) {
    return NumericKind::Float64;
  }
  if (containsWord(dataEvalOutput, "float")) {
    return NumericKind::Float32;
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
    if (std::isnan(val)) {
      sawNaN = true;
      continue;
    }
    if (val < minVal || std::isnan(minVal)) {
      minVal = val;
    }
    if (val > maxVal || std::isnan(maxVal)) {
      maxVal = val;
    }
    if (std::isinf(val)) {
      sawInf = true;
      continue;
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

void publishSummary(
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view expr,
    const SummaryStats& stats) {
  if (host->log_message != nullptr) {
    char logBuf[512]{};
    const char* elemLabel = (stats.kind == NumericKind::Float32) ? "f32" : "f64";
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
    (void)host->log_message(host->host_ctx, GRIDLOCK_LOG_INFO, logBuf);
  }

  if (host->emit_structured_event != nullptr) {
    const std::string jsonPayload =
        std::string("{\"rank\":") + std::to_string(rank) +
        ",\"expr\":\"" + escapeJson(expr) + "\"" +
        ",\"count\":" + std::to_string(stats.sampled_count) +
        ",\"min\":" + formatJsonNumber(stats.min_val) +
        ",\"max\":" + formatJsonNumber(stats.max_val) +
        ",\"mean\":" + formatJsonNumber(stats.mean_val) +
        ",\"l2_norm\":" + formatJsonNumber(stats.l2_norm) + "}";
    (void)host->emit_structured_event(host->host_ctx, kTelemetryChannel, jsonPayload.c_str());
  }
}

[[nodiscard]] GridLockStatus inspectContainerAndPublish(
    const GridLockHostApi* host,
    int32_t rank,
    std::string_view rawExpr,
    std::string_view typeHint) {
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
  const std::string_view dataView = dataStrRes.has_value() ? std::string_view(*dataStrRes) : std::string_view{};
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
    if (kind == NumericKind::Float32) {
      std::vector<float> buffer(targetCount, 0.0f);
      const auto readRes =
          gp::readBytes(host, rank, dataAddr, std::as_writable_bytes(std::span<float>(buffer)));
      if (!readRes.has_value()) {
        return readRes.error();
      }
      const std::size_t elemsRead = (*readRes) / sizeof(float);
      if (elemsRead == 0u) {
        return GRIDLOCK_ERR_MEMORY_READ_FAILED;
      }
      stats = reduceSpan<float>(std::span<const float>(buffer.data(), elemsRead), totalCount, kind);
    } else {
      std::vector<double> buffer(targetCount, 0.0);
      const auto readRes =
          gp::readBytes(host, rank, dataAddr, std::as_writable_bytes(std::span<double>(buffer)));
      if (!readRes.has_value()) {
        return readRes.error();
      }
      const std::size_t elemsRead = (*readRes) / sizeof(double);
      if (elemsRead == 0u) {
        return GRIDLOCK_ERR_MEMORY_READ_FAILED;
      }
      stats = reduceSpan<double>(std::span<const double>(buffer.data(), elemsRead), totalCount, kind);
    }
  }

  publishSummary(host, rank, expr, stats);
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

    return inspectContainerAndPublish(host, varCtx->rank, state->tracked_expr, state->tracked_type_hint);
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
      return inspectContainerAndPublish(host, rank, state->tracked_expr, state->tracked_type_hint);
    }

    // No container tracked yet: report stop status cleanly on both log and telemetry channels.
    if (host->log_message != nullptr) {
      char logBuf[384]{};
      const char* fn = state->last_stop_function.empty() ? "<unknown>" : state->last_stop_function.c_str();
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
      (void)host->emit_structured_event(host->host_ctx, kTelemetryChannel, jsonPayload.c_str());
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
    state->last_stopped_rank = -1;
    state->last_stop_line = -1;
    state->last_stop_reason.clear();
    state->last_stop_function.clear();
    state->stop_events_seen = 0u;

    if (host != nullptr && host->log_message != nullptr) {
      const std::string msg =
          "[stl-inspector] initialized session across " + std::to_string(rankCount) + " MPI ranks";
      (void)host->log_message(host->host_ctx, GRIDLOCK_LOG_DEBUG, msg.c_str());
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
    state->last_stop_function = (stopInfo->function_utf8 != nullptr) ? stopInfo->function_utf8 : "";
    ++state->stop_events_seen;

    if (!state->tracked_expr.empty()) {
      (void)inspectContainerAndPublish(host, stopInfo->rank, state->tracked_expr, state->tracked_type_hint);
    }
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
  }
}

GridLockStatus GRIDLOCK_CALL onSessionEnd(
    void* pluginCtx,
    const GridLockHostApi* /*host*/) noexcept {
  try {
    auto* state = static_cast<PluginState*>(pluginCtx);
    if (state == nullptr) {
      return GRIDLOCK_ERR_INVALID_ARG;
    }
    state->last_stopped_rank = -1;
    state->last_stop_line = -1;
    state->last_stop_reason.clear();
    state->last_stop_function.clear();
    state->stop_events_seen = 0u;
    return GRIDLOCK_OK;
  } catch (...) {
    return GRIDLOCK_ERR_UNSUPPORTED;
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

    outDesc->plugin_id_utf8 = kPluginId;
    outDesc->plugin_name_utf8 = kPluginName;
    outDesc->plugin_version_utf8 = kPluginVersion;
    outDesc->description_utf8 = kPluginDescription;
    outDesc->plugin_ctx = state.release();
    outDesc->on_session_start = &onSessionStart;
    outDesc->on_rank_stopped = &onRankStopped;
    outDesc->on_session_end = &onSessionEnd;
    outDesc->shutdown = &onShutdown;
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
