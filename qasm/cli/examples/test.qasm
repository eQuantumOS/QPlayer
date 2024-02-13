// QuvieC initial code
OPENQASM 2.0;
include "qelib1.inc";

qreg q[3];
creg c[3];

id q[0];
u1 (pi/2) q[1];
u2 (pi/2, pi/3) q[1];
u3 (pi/2, pi/3, pi/4) q[1];
x q[0];
y q[0];
z q[0];
h q[0];
p q[0];
s q[0];
t q[0];
sdg q[0];
tdg q[0];
rx (pi/2) q[0];
ry (pi/2) q[0];
rz (pi/2) q[0];
cx q[0],q[1];
cz q[0],q[1];
cy q[0],q[1];
ch q[0],q[1];
ccx q[0],q[1],q[2];
crz (pi/2) q[0],q[1];
cu1 (pi/2) q[0],q[1];
cu2 (pi/2, pi/3) q[0],q[1];
cu3 (pi/2, pi/3, pi/4) q[0],q[1];
swap q[0],q[1];
cswap q[0],q[1],q[2];
sx q[0];
sxdg q[0];
iswap q[0],q[1];
