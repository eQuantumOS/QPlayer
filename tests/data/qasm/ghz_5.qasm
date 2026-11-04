// ghz_5.qasm — 5-qubit GHZ state acceptance circuit (Phase 5 gate target).
// Exercises multi-qubit CX chains. Expected distribution:
//   |00000>=0.5, |11111>=0.5, tol=5% at nshots=10000.
// Linked to tests/qasm/expected/ghz_5.json.

OPENQASM 2.0;
include "qelib1.inc";

qreg q[5];
creg c[5];

h q[0];
cx q[0], q[1];
cx q[1], q[2];
cx q[2], q[3];
cx q[3], q[4];

measure q[0] -> c[0];
measure q[1] -> c[1];
measure q[2] -> c[2];
measure q[3] -> c[3];
measure q[4] -> c[4];
