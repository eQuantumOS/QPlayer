// GHZ(5) — fills FakeManilaV2 (5 qubits); a linear CX chain that forces SWAP
// routing on devices whose coupling map is not a simple line. Ideal 50/50 on
// |00000>/|11111>; the deeper circuit accumulates more relaxation + gate error.
OPENQASM 2.0;
include "qelib1.inc";
qreg q[5];
creg c[5];
h q[0];
cx q[0],q[1];
cx q[1],q[2];
cx q[2],q[3];
cx q[3],q[4];
measure q[0] -> c[0];
measure q[1] -> c[1];
measure q[2] -> c[2];
measure q[3] -> c[3];
measure q[4] -> c[4];
