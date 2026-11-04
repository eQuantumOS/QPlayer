/// @file qelib1_builtin.cpp
/// @brief compile-time validation for the qelib1 built-in table
///
///
///
/// The two compile-time tables (kQelib1Signatures in qelib1_signatures.hpp and
/// kQelib1Mappings in qelib1_builtin.hpp) must stay in lock-step. This TU owns
/// the static_assert that enforces their sizes are equal so a future extension
/// that adds an entry to one table but not the other is caught at build time.
///
/// No runtime code lives here beyond the static assertion — all lookup logic is
/// implemented inline in the respective headers.

#include "parser/detail/qelib1_builtin.hpp"
#include "parser/detail/qelib1_signatures.hpp"

// Enforce 1:1 correspondence between the two compile-time tables. If either
// table is extended without updating the other, this build will fail with a
// meaningful message pointing to the right files.
static_assert(
    qplayer::detail::kQelib1Mappings.size() ==
    qplayer::detail::kQelib1Signatures.size(),
    "kQelib1Mappings and kQelib1Signatures must have the same number of entries "
    "(qelib1_builtin.hpp vs qelib1_signatures.hpp). Update both tables together.");
