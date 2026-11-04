// alap_schedule.hpp — ALAPSchedulePass placeholder (Phase 8).
// As-Late-As-Possible scheduling that prepares each qubit's idle windows for
// IdleNoiseInserterPass.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

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
