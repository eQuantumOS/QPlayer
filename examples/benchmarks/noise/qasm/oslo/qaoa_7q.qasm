// QAOA MaxCut on a line graph (edges 0-1..5-6), p=1: Hadamard
// layer, ZZ cost phase per edge (cx-rz-cx, rz angle -2*gamma so the cost
// rewards cuts), then an RX mixer. 7 qubits; gamma=0.6, beta=0.4
// (fixed representative angles). The ideal run peaks on the alternating MaxCut
// solutions; relaxation + gate error + readout spread weight off those peaks.
OPENQASM 2.0;
include "qelib1.inc";
qreg q[7];
creg c[7];
h q[0];
h q[1];
h q[2];
h q[3];
h q[4];
h q[5];
h q[6];
cx q[0],q[1];
rz(-1.200000) q[1];
cx q[0],q[1];
cx q[1],q[2];
rz(-1.200000) q[2];
cx q[1],q[2];
cx q[2],q[3];
rz(-1.200000) q[3];
cx q[2],q[3];
cx q[3],q[4];
rz(-1.200000) q[4];
cx q[3],q[4];
cx q[4],q[5];
rz(-1.200000) q[5];
cx q[4],q[5];
cx q[5],q[6];
rz(-1.200000) q[6];
cx q[5],q[6];
rx(0.800000) q[0];
rx(0.800000) q[1];
rx(0.800000) q[2];
rx(0.800000) q[3];
rx(0.800000) q[4];
rx(0.800000) q[5];
rx(0.800000) q[6];
measure q[0] -> c[0];
measure q[1] -> c[1];
measure q[2] -> c[2];
measure q[3] -> c[3];
measure q[4] -> c[4];
measure q[5] -> c[5];
measure q[6] -> c[6];
