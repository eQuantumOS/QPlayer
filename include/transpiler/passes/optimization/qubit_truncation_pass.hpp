// qubit_truncation_pass.hpp — QubitTruncationPass (optimization/truncation stage,
//). After layout/routing/translation place a circuit on a
// (possibly wide) device, the referenced physical qubits may be sparse (e.g. a
// 7-qubit circuit mapped onto qubits {0,5,11,...,26} of a 27-qubit device).
// This pass compacts the actually-referenced physical qubits into a dense
// [0, k) range, rewrites every op's qubit operands, and reduces
// Circuit::num_qubits to k so the statevector is 2^k instead of 2^(device width).
//
// counts stay user-perspective automatically: MeasureOp.cubit (the classical
// register bit) is never touched, only MeasureOp.qubit is relabelled. The
// dense->physical mapping is stamped into CircuitMetadata.qubit_truncation so
// noise synthesis can look up device T1/T2/error by the
// original physical qubit.
//
// Placement: device-bound transpile() only,
// after CouplingValidator (which must validate 2Q adjacency in physical space)
// and before scheduling (so duration/idle-noise bind to the compacted circuit).
// A no-op — leaving num_qubits and CircuitMetadata untouched — when the
// referenced qubits are already the dense set [0, num_qubits).

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class QubitTruncationPass : public IPass {
public:
    QubitTruncationPass() = default;

    std::string name()    const override;
    // No throw: purely a relabelling of already-valid qubit indices.
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
