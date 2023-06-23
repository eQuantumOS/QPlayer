// QuvieC initial code
OPENQASM 2.0;
include "qelib1.inc";

qreg q[4];
creg c[4];

h q;
rx(pi/2) q[0];
ry(pi/3) q[1];
rz(pi/4) q[2];

s q;
t q;
tdg q;
sdg q;

measure q -> c;
