// bell.qasm — Bell pair acceptance circuit (Phase 5 gate target).
// Verifies the basic Batch pipeline: parse -> transpile -> run on FullState.
// Expected distribution: |00>=0.5, |11>=0.5, tol=5% at nshots=10000.
// Linked to tests/qasm/expected/bell.json. Reference: acceptance section.

OPENQASM 2.0;
include "qelib1.inc";

qreg q[2];
creg c[2];

h q[0];
cx q[0], q[1];

measure q -> c;
