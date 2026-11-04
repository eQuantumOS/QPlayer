/// @file parameter.hpp
/// @brief Symbolic / numeric gate parameter scaffold.
/// @ingroup ir
///
/// Phase 0-S keeps the type minimal — only the numeric path is exercised by the
/// initial gates. Symbolic Expression nodes and the variant<double, Expression>
/// from land in Phase 4 once parameter binding lights up.

#pragma once

#include <string>

namespace qplayer {

/// Placeholder for the Parameter ADT. Once symbolic expressions arrive (Phase 4)
/// the type morphs into variant<double, Expression> as described in
/// Until then GateOp stores `std::vector<double>` directly
/// and this struct documents the migration path.
struct Parameter {
    std::string name;          // Empty when purely numeric.
    double      value       = 0.0;
    bool        is_symbolic = false;
};

}  // namespace qplayer
