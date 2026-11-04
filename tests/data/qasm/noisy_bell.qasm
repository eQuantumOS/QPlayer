// noisy_bell.qasm — Bell pair under depolarizing + amplitude-damping noise
// (Phase 8 gate target). Same circuit topology as bell.qasm; the noise model
// is supplied at runtime via QRegister::set_noise_model (not inside QASM).
//
// Expected fidelity vs ideal Bell state: theory +/- 0.005 at nshots=100,000.
// Theory derivation lives in tests/qasm/expected/noisy_bell.json
// (depolarizing p=1e-3 per single-qubit gate, p=5e-3 per two-qubit gate).

OPENQASM 2.0;
include "qelib1.inc";

qreg q[2];
creg c[2];

h q[0];
cx q[0], q[1];

measure q[0] -> c[0];
measure q[1] -> c[1];
