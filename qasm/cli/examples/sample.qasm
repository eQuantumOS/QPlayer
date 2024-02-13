// QuvieC initial code
OPENQASM 2.0;
include "qelib1.inc";

qreg q[4];
creg c[4];
creg c2[3];

gate ugate1(r1, r2, r3) a, b, c {
	cx a, b;
	h c;
	u3(r1, r2, r3) c;
}

gate ugate2 a, b {
	cx a, b;
	x a;
}

h q;
u1(cos(pi/3)/0.3*sin(10.2)) q[2];
u2(pi/10, cos(pi/3)/0.3*sin(10.2)) q[2];

u3(pi/4, -pi/10, pi/15) q[2];
u2(pi/10, cos(pi/3)/0.3*sin(10.2)) q[2];
rz(cos(pi/3)/0.3*sin(10.2)) q[2];
u2(pi/10, cos(pi/3)/0.3*sin(10.2)) q[2];

ugate1 (pi/2, pi/3, pi/4) q[0], q[1], q[2];
ugate2 q[0], q[1];

measure q -> c;
measure q[0] -> c [1];

if (c == 1) u2(pi/10, cos(pi/3)/0.3*sin(10.2)) q[2];
if (c <= 2) h q;
if (c < 3) cx q[0],q[2];
if (c2 >= 4) ccx q[0],q[1], q[2];
if (c == 0) rx(pi) q;
