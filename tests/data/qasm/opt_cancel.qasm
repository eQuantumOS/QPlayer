// opt_cancel.qasm — CancelInversePass acceptance (Phase 4 gate target).
// Inserts 6 self-inverse gate pairs across 3 qubits, plus a single un-paired
// H q[0]. After transpile(optimization_level=1), only that lone H should
// survive -> circuit.num_ops() == 1 (no backend execution required).
//
// Cancellation pairs:
//   H q[0] -- H q[0]    (the second pair survives as the lone op below; only
//                         even pairs of H cancel cleanly)
//   X q[1] -- X q[1]
//   Z q[2] -- Z q[2]
//   CX q[0],q[1] -- CX q[0],q[1]
// Linked to tests/qasm/expected/opt_cancel.json.

OPENQASM 2.0;
include "qelib1.inc";

qreg q[3];
creg c[3];

// Lone gate -- expected to survive cancellation (num_ops == 1).
h q[0];

// Self-inverse pairs that CancelInversePass must collapse to nothing.
x q[1];
x q[1];

z q[2];
z q[2];

cx q[0], q[1];
cx q[0], q[1];

h q[2];
h q[2];
