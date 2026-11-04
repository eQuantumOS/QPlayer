OPENQASM 2.0;
include "qelib1.inc";

qreg q[20];
creg c[20];

h q;

// depth 01
id q[0];
x q[1];
y q[2];
z q[3];
h q[4];
s q[5];
sdg q[6];
t q[7];
tdg q[8];
rx(pi/7) q[9];
ry(pi/5) q[10];
rz(pi/3) q[11];
u1(pi/9) q[12];
u2(pi/8, pi/6) q[13];
u3(pi/4, pi/7, pi/9) q[14];
barrier q;

// depth 02
cx q[0], q[1];
cy q[2], q[3];
cz q[4], q[5];
ch q[6], q[7];
swap q[8], q[9];
crz(pi/11) q[10], q[11];
cu1(pi/13) q[12], q[13];
cu3(pi/5, pi/7, pi/9) q[14], q[15];
barrier q;

// depth 03
ccx q[0], q[1], q[2];
cswap q[3], q[4], q[5];
rx(pi/12) q[6];
ry(pi/10) q[7];
rz(pi/8) q[8];
x q[9];
y q[10];
z q[11];
h q[12];
s q[13];
t q[14];
tdg q[15];
barrier q;

// depth 04
cx q[16], q[17];
cz q[18], q[19];
u1(pi/4) q[0];
u2(pi/3, pi/5) q[1];
u3(pi/6, pi/8, pi/10) q[2];
sdg q[6];
id q[7];
barrier q;

// depth 05
swap q[0], q[19];
cy q[1], q[18];
ch q[2], q[17];
crz(pi/6) q[3], q[16];
cu1(pi/7) q[4], q[15];
cu3(pi/9, pi/10, pi/11) q[5], q[14];
barrier q;

// depth 06
ccx q[6], q[7], q[8];
cswap q[9], q[10], q[11];
rx(pi/13) q[12];
ry(pi/14) q[13];
rz(pi/15) q[14];
h q[15];
s q[16];
sdg q[17];
t q[18];
tdg q[19];
barrier q;

// depth 07
cx q[0], q[2];
cx q[3], q[5];
cz q[6], q[8];
cy q[9], q[11];
swap q[12], q[14];
ch q[15], q[17];
barrier q;

// depth 08
u3(pi/7, pi/8, pi/9) q[0];
u2(pi/6, pi/4) q[3];
u1(pi/5) q[6];
rx(pi/9) q[9];
ry(pi/8) q[12];
rz(pi/7) q[15];
x q[18];
z q[19];
barrier q;

// depth 09
ccx q[0], q[3], q[6];
cswap q[9], q[12], q[15];
cu1(pi/12) q[1], q[4];
crz(pi/10) q[7], q[10];
barrier q;

// depth 10
cu3(pi/8, pi/9, pi/10) q[2], q[5];
cx q[8], q[11];
cy q[13], q[16];
cz q[17], q[19];
h q[0];
s q[1];
t q[3];
barrier q;

// depth 11
swap q[4], q[6];
ch q[7], q[9];
rx(pi/16) q[10];
ry(pi/17) q[11];
rz(pi/18) q[12];
u1(pi/19) q[13];
u2(pi/20, pi/21) q[14];
u3(pi/22, pi/23, pi/24) q[15];
barrier q;

// depth 12
ccx q[16], q[17], q[18];
cswap q[0], q[1], q[2];
x q[3];
y q[4];
z q[5];
h q[6];
sdg q[7];
tdg q[8];
barrier q;

// depth 13
cx q[5], q[10];
cy q[6], q[11];
cz q[7], q[12];
ch q[8], q[13];
swap q[9], q[14];
crz(pi/9) q[15], q[19];
barrier q;

// depth 14
cu1(pi/8) q[0], q[4];
cu3(pi/7, pi/6, pi/5) q[1], q[5];
rx(pi/4) q[2];
ry(pi/3) q[3];
rz(pi/2) q[6];
s q[7];
t q[8];
barrier q;

// depth 15
ccx q[10], q[11], q[12];
cswap q[13], q[14], q[15];
u1(pi/6) q[16];
u2(pi/7, pi/8) q[17];
u3(pi/9, pi/10, pi/11) q[18];
id q[19];
barrier q;

// depth 16
cx q[0], q[10];
cy q[1], q[11];
cz q[2], q[12];
ch q[3], q[13];
swap q[4], q[14];
crz(pi/13) q[5], q[15];
barrier q;

// depth 17
cu1(pi/14) q[6], q[16];
cu3(pi/15, pi/16, pi/17) q[7], q[17];
ccx q[8], q[18], q[19];
rx(pi/18) q[0];
ry(pi/19) q[1];
rz(pi/20) q[2];
barrier q;

// depth 18
cswap q[3], q[9], q[15];
h q[4];
x q[5];
y q[6];
z q[7];
s q[8];
sdg q[10];
t q[11];
tdg q[12];
barrier q;

// depth 19
cx q[13], q[14];
cy q[15], q[16];
cz q[17], q[18];
ch q[0], q[19];
swap q[1], q[2];
crz(pi/21) q[3], q[4];
barrier q;

// depth 20
cu1(pi/22) q[5], q[6];
cu3(pi/23, pi/24, pi/25) q[7], q[8];
ccx q[9], q[10], q[11];
u1(pi/26) q[12];
u2(pi/27, pi/28) q[13];
u3(pi/29, pi/30, pi/31) q[14];
barrier q;

// depth 21
cswap q[16], q[17], q[18];
rx(pi/6) q[0];
ry(pi/7) q[1];
rz(pi/8) q[2];
h q[3];
x q[4];
y q[5];
z q[6];
barrier q;

// depth 22
cx q[7], q[9];
cy q[8], q[10];
cz q[11], q[13];
ch q[12], q[14];
swap q[15], q[19];
crz(pi/32) q[0], q[16];
barrier q;

// depth 23
cu1(pi/33) q[1], q[17];
cu3(pi/34, pi/35, pi/36) q[2], q[18];
ccx q[3], q[4], q[5];
s q[6];
sdg q[7];
t q[8];
tdg q[9];
barrier q;

// depth 24
cswap q[10], q[11], q[12];
u1(pi/10) q[13];
u2(pi/11, pi/12) q[14];
u3(pi/13, pi/14, pi/15) q[15];
rx(pi/16) q[16];
ry(pi/17) q[17];
rz(pi/18) q[18];
id q[19];
barrier q;

// depth 25
cx q[0], q[5];
cy q[1], q[6];
cz q[2], q[7];
ch q[3], q[8];
swap q[4], q[9];
crz(pi/19) q[10], q[15];
barrier q;

// depth 26
cu1(pi/20) q[11], q[16];
cu3(pi/21, pi/22, pi/23) q[12], q[17];
ccx q[13], q[18], q[19];
h q[0];
x q[1];
y q[2];
z q[3];
barrier q;

// depth 27
cswap q[4], q[5], q[6];
s q[7];
sdg q[8];
t q[9];
tdg q[10];
rx(pi/24) q[11];
ry(pi/25) q[12];
rz(pi/26) q[13];
barrier q;

// depth 28
cx q[14], q[15];
cy q[16], q[17];
cz q[18], q[19];
ch q[0], q[1];
swap q[2], q[3];
crz(pi/27) q[4], q[5];
barrier q;

// depth 29
cu1(pi/28) q[6], q[7];
cu3(pi/29, pi/30, pi/31) q[8], q[9];
ccx q[10], q[11], q[12];
cswap q[13], q[14], q[15];
u1(pi/32) q[16];
u2(pi/33, pi/34) q[17];
u3(pi/35, pi/36, pi/37) q[18];
barrier q;

// depth 30
rx(pi/38) q[0];
ry(pi/39) q[1];
rz(pi/40) q[2];
h q[3];
x q[4];
y q[5];
z q[6];
s q[7];
sdg q[8];
t q[9];
tdg q[10];
id q[11];
