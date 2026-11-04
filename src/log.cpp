// log.cpp — qplayer::log facade — real implementation.
// Phase 0-S kept the dispatchers as no-ops; Phase 1 replaces them with:
// 1. One-time env-var bootstrap via std::call_once.
//   2. current_level() gate so sub-threshold messages are free.
// 3. std::clog output serialized with std::mutex.
// Format: "[QPlayer/<LEVEL>] <message>\n"

#include "log.hpp"

#include <atomic>
#include <cstdlib>     // std::getenv
#include <iostream>    // std::clog
#include <mutex>
#include <string_view>

namespace qplayer::log {

namespace {

// ── Level storage ─────────────────────────────────────────────────────────

// Atomic so concurrent set_level() / current_level() from OpenMP regions is
// well-defined.
std::atomic<Level> g_level{Level::Warn};

// ── Env-var bootstrap (one-time, thread-safe) ────────────────────────────

// Parses QPLAYER_LOG_LEVEL (case-insensitive). Unknown values fall back to
// the default warn level.
void parse_env_level() {
    const char* raw = std::getenv("QPLAYER_LOG_LEVEL");
    if (!raw) return;

    std::string_view val(raw);

    // Case-insensitive comparison helper.
    auto ci_eq = [&](std::string_view a, const char* b) -> bool {
        std::string_view bv(b);
        if (a.size() != bv.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ac = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
            char bc = (bv[i] >= 'A' && bv[i] <= 'Z') ? static_cast<char>(bv[i] + 32) : bv[i];
            if (ac != bc) return false;
        }
        return true;
    };

    if      (ci_eq(val, "debug")) g_level.store(Level::Debug, std::memory_order_relaxed);
    else if (ci_eq(val, "info"))  g_level.store(Level::Info,  std::memory_order_relaxed);
    else if (ci_eq(val, "warn"))  g_level.store(Level::Warn,  std::memory_order_relaxed);
    else if (ci_eq(val, "error")) g_level.store(Level::Error, std::memory_order_relaxed);
    else if (ci_eq(val, "off"))   g_level.store(Level::Off,   std::memory_order_relaxed);
    // Unknown value: keep the default (Warn).
}

// std::once_flag ensures the env probe runs exactly once even under concurrent
// first-call scenarios (e.g. OpenMP workers all calling log::warn together).
std::once_flag g_init_flag;

void ensure_initialized() {
    std::call_once(g_init_flag, parse_env_level);
}

// ── Output mutex ───────────────────────────────────────────

// Guards std::clog writes so each message appears as an atomic line even when
// multiple OpenMP threads call log::warn concurrently.
std::mutex g_clog_mutex;

// Level -> uppercase 5-char label.
const char* level_tag(Level lvl) {
    switch (lvl) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        default:           return "?????";
    }
}

// Core emit: gate on level, then write one atomic line to std::clog.
void emit(Level lvl, std::string_view msg) {
    ensure_initialized();
    if (static_cast<int>(lvl) < static_cast<int>(g_level.load(std::memory_order_relaxed))) {
        return;
    }
    if (lvl == Level::Off) return;

    // Build message before locking to minimize lock hold time.
    // Format: "[QPlayer/LEVEL] message\n"
    std::string line;
    line.reserve(msg.size() + 20);
    line += "[QPlayer/";
    line += level_tag(lvl);
    line += "] ";
    line += msg;
    line += '\n';

    {
        std::lock_guard<std::mutex> guard(g_clog_mutex);
        std::clog << line;
    }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

Level current_level() {
    return g_level.load(std::memory_order_relaxed);
}

void set_level(Level lvl) {
    g_level.store(lvl, std::memory_order_relaxed);
}

void debug(std::string_view msg) { emit(Level::Debug, msg); }
void info (std::string_view msg) { emit(Level::Info,  msg); }
void warn (std::string_view msg) { emit(Level::Warn,  msg); }
void error(std::string_view msg) { emit(Level::Error, msg); }

}  // namespace qplayer::log
