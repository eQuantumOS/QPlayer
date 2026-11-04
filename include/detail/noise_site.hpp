/// @file noise_site.hpp
/// @brief Per-trajectory noise-site counter for interventions and the draw ledger.
/// @ingroup core
///
/// Shared by the FullState CPU and TensorNetwork trajectory loops so both number
/// sites the same way. The caller draws the site's uniform itself and asks the
/// cursor whether to skip the channel; the draw is therefore consumed whether or
/// not the site is intervened, which keeps every later draw aligned.

#pragma once

#include "noise/noise_intervention.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qplayer {
namespace detail {

class NoiseSiteCursor {
public:
    /// `sites` must be sorted; `ledger` is null when the ledger is off.
    NoiseSiteCursor(const std::vector<std::uint32_t>& sites,
                    std::vector<NoiseDrawEvent>* ledger) noexcept
        : sites_(sites), ledger_(ledger) {}

    /// Registers the next site; returns true when it is intervened, in which case
    /// the caller must not apply the channel. `u` is the uniform already drawn.
    bool next(std::size_t op_index, double u) {
        const std::uint32_t s = count_++;
        const bool hit = !sites_.empty() &&
                         std::binary_search(sites_.begin(), sites_.end(), s);
        if (ledger_ != nullptr) ledger_->push_back({s, op_index, 0u, u, hit});
        return hit;
    }

    /// Records the branch taken by the site last registered (ledger only).
    void set_branch(std::size_t branch) noexcept {
        if (ledger_ != nullptr && !ledger_->empty())
            ledger_->back().branch = static_cast<std::uint32_t>(branch);
    }

    std::uint32_t count() const noexcept { return count_; }

private:
    const std::vector<std::uint32_t>& sites_;
    std::vector<NoiseDrawEvent>* ledger_;
    std::uint32_t count_ = 0;
};

/// Requested sites the run never reached are a caller bug, not a no-op: a typo
/// would otherwise silently become "no intervention". Returns the
/// first offending site, or -1 when every site is in range.
inline std::int64_t first_unreached_site(const std::vector<std::uint32_t>& sorted_sites,
                                         const std::vector<std::uint32_t>& per_shot_counts) {
    if (sorted_sites.empty()) return -1;
    std::uint32_t max_count = 0;
    for (std::uint32_t c : per_shot_counts) max_count = std::max(max_count, c);
    return sorted_sites.back() >= max_count ? static_cast<std::int64_t>(sorted_sites.back())
                                            : -1;
}

}  // namespace detail
}  // namespace qplayer
