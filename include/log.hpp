/// @file log.hpp
/// @brief QPlayer process-wide logging facade.
/// @ingroup core
///
/// All diagnostic output funnels through this header.
/// forbids direct std::cerr / std::clog use outside src/log.cpp.

#pragma once

#include <string_view>

namespace qplayer::log {

/// Severity levels. Order matters: messages below current_level() are dropped.
/// QPLAYER_LOG_LEVEL env (read once on first call) selects the initial level;
/// set_level() overrides dynamically (testing).
enum class Level {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Off   = 4,
};

/// Message dispatchers. Caller assembles the string (no variadic format) so the
/// signature stays trivial and ABI-stable across pybind11 boundary.
void debug(std::string_view msg);
void info (std::string_view msg);
void warn (std::string_view msg);
void error(std::string_view msg);

/// Level inspection / override. set_level() is the only runtime mutation path —
/// env var QPLAYER_LOG_LEVEL is cached once on process start.
Level current_level();
void  set_level(Level lvl);

}  // namespace qplayer::log
