/// @file alap_schedule.hpp
/// @brief As-Late-As-Possible scheduling that derives per-qubit idle windows.
/// @ingroup transpiler
///
/// As-Late-As-Possible scheduling that prepares each qubit's idle windows for
/// IdleNoiseInserterPass. That pass was removed
/// in Phase 8-F — this is now the only idle-noise inserter.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Computes idle windows and inserts the IdleOps that carry relaxation noise; the sole idle-noise inserter since Phase 8-F.
/// @ingroup transpiler
class ALAPSchedulePass : public IPass {
public:
    ALAPSchedulePass() = default;

    std::string name() const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
