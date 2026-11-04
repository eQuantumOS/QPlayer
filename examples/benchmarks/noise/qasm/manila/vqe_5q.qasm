// VQE ansatz (RealAmplitudes-style, reps=2): an RY rotation layer
// followed by reps x (linear CX entangler + RY layer), 5 qubits. Angles
// are frozen from seed 1234 (a representative point, not an optimized
// minimum). The two entangling layers accumulate relaxation + 2q gate error
// so the measured distribution departs visibly from the ideal one.
OPENQASM 2.0;
include "qelib1.inc";
qreg q[5];
creg c[5];
ry(5.082125) q[0];
ry(1.345940) q[1];
ry(4.472593) q[2];
ry(4.736546) q[3];
ry(3.593450) q[4];
cx q[0],q[1];
cx q[1],q[2];
cx q[2],q[3];
cx q[3],q[4];
ry(0.571739) q[0];
ry(0.082773) q[1];
ry(5.056776) q[2];
ry(5.890900) q[3];
ry(1.538158) q[4];
cx q[0],q[1];
cx q[1],q[2];
cx q[2],q[3];
cx q[3],q[4];
ry(2.593325) q[0];
ry(5.288541) q[1];
ry(0.580682) q[2];
ry(1.635608) q[3];
ry(5.053296) q[4];
measure q[0] -> c[0];
measure q[1] -> c[1];
measure q[2] -> c[2];
measure q[3] -> c[3];
measure q[4] -> c[4];
