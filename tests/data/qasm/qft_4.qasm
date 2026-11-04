// qft_4.qasm — 4-qubit Quantum Fourier Transform on |0000> input.
// Exercises parametric Rz / controlled-phase gates. Expected check:
//   statevector amplitude L2-norm error <= 1e-10 vs analytic QFT|0000>
//   (uniform superposition with amplitude 1/4 = 0.25).
// nshots=1 (statevector-mode verification). Linked to tests/qasm/expected/qft_4.json.

OPENQASM 2.0;
include "qelib1.inc";

qreg q[4];
creg c[4];

// Apply 4-qubit QFT (no input rotation -> uniform output state).
h q[0];
cu1(pi/2) q[1], q[0];
cu1(pi/4) q[2], q[0];
cu1(pi/8) q[3], q[0];

h q[1];
cu1(pi/2) q[2], q[1];
cu1(pi/4) q[3], q[1];

h q[2];
cu1(pi/2) q[3], q[2];

h q[3];

// Bit-reversal swaps to match standard QFT output ordering.
swap q[0], q[3];
swap q[1], q[2];
