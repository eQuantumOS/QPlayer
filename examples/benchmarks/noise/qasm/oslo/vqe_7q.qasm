// VQE ansatz (RealAmplitudes-style, reps=2): an RY rotation layer
// followed by reps x (linear CX entangler + RY layer), 7 qubits. Angles
// are frozen from seed 1234 (a representative point, not an optimized
// minimum). The two entangling layers accumulate relaxation + 2q gate error
// so the measured distribution departs visibly from the ideal one.
OPENQASM 2.0;
include "qelib1.inc";
qreg q[7];
creg c[7];
ry(0.966642) q[0];
ry(3.907650) q[1];
ry(4.742997) q[2];
ry(4.621232) q[3];
ry(6.181004) q[4];
ry(0.192559) q[5];
ry(3.066186) q[6];
cx q[0],q[1];
cx q[1],q[2];
cx q[2],q[3];
cx q[3],q[4];
cx q[4],q[5];
cx q[5],q[6];
ry(3.231133) q[0];
ry(1.850388) q[1];
ry(6.262249) q[2];
ry(3.299952) q[3];
ry(0.077043) q[4];
ry(5.199097) q[5];
ry(3.016295) q[6];
cx q[0],q[1];
cx q[1],q[2];
cx q[2],q[3];
cx q[3],q[4];
cx q[4],q[5];
cx q[5],q[6];
ry(0.846979) q[0];
ry(1.176941) q[1];
ry(6.180421) q[2];
ry(5.110817) q[3];
ry(1.473921) q[4];
ry(3.154459) q[5];
ry(5.713020) q[6];
measure q[0] -> c[0];
measure q[1] -> c[1];
measure q[2] -> c[2];
measure q[3] -> c[3];
measure q[4] -> c[4];
measure q[5] -> c[5];
measure q[6] -> c[6];
