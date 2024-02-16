// QuvieC initial code
OPENQASM 2.0;
include "qelib1.inc";

qreg q[4];
creg c[4];

gate mygate (r1) a b {
    h a;
    u1 (r1) b;
}

x q[0];
cx q[0], q[1];
mygate (pi) q[0], q[2];

measure q -> c;

if (c == 0) x q[1];
