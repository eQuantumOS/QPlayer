/// @file noise_intervention.hpp
/// @brief One record of the per-shot noise draw ledger.
/// @ingroup noise

#pragma once

#include <cstddef>
#include <cstdint>

namespace qplayer {

/// One noise site of one trajectory: the uniform draw it consumed and what it did.
///
/// A site is one uniform the trajectory consumes because of the NoiseModel -- a
/// gate-, reset- or idle-attached Kraus channel, or a readout confusion. Sites are
/// numbered per shot from 0 in the noise-trajectory canonical execution order
/// so the ledger order IS the site order. Born collapses of measure and
/// reset, opaque Kraus bodies and the TN sampler draw are not sites.
struct NoiseDrawEvent {
    /// Site number in canonical execution order (the index QRegister::set_noise_interventions takes).
    std::uint32_t site = 0;
    /// Index into the executed circuit's ops() of the op that owns the site.
    std::size_t op_index = 0;
    /// Kraus branch chosen; for readout, 1 if the reported bit was flipped; 0 when intervened.
    std::uint32_t branch = 0;
    /// The uniform the site consumed -- identical with and without an intervention.
    double u = 0.0;
    /// Whether the site was replaced by the identity.
    bool intervened = false;
};

}  // namespace qplayer
