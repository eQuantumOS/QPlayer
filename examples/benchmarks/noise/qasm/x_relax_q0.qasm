// Single-qubit relaxation + readout probe on qubit 0. Prepare |1> with X, then
// measure. Ideal result is |1> with probability 1; on hardware the X-gate error,
// T1 amplitude damping over the gate duration, and the asymmetric readout error
// (P(0|1)) all pull measured weight toward |0>. The most direct circuit-level
// witness of qubit 0's noise parameters that the T1/T2/T2* table reports.
OPENQASM 2.0;
include "qelib1.inc";
qreg q[1];
creg c[1];
x q[0];
measure q[0] -> c[0];
