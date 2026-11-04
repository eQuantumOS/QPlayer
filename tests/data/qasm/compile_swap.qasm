// compile_swap.qasm — F-CC SWAP routing acceptance (Phase 8 gate target).
// Logical 2-qubit gate between q[0] and q[3] forces SWAP insertion when the
// device coupling map is a linear chain (0-1-2-3). After transpile() the
// physical circuit contains additional SWAPs; run() output must still be
// correct relative to the LOGICAL qubits.
//
// Expected: |0011>=0.5, |1100>=0.5 on logical qubits (post-SWAP-unwind),
// tol=5% at nshots=10000. Linked to tests/qasm/expected/compile_swap.json.

OPENQASM 2.0;
include "qelib1.inc";

qreg q[4];
creg c[4];

h q[0];
// Logical CX between non-adjacent qubits on a linear coupling map.
cx q[0], q[3];

x q[1];
x q[2];

measure q[0] -> c[0];
measure q[1] -> c[1];
measure q[2] -> c[2];
measure q[3] -> c[3];
