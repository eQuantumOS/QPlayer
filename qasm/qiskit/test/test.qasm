OPENQASM 2.0;
include "qelib1.inc";

qreg q[25];
creg c[25];

h q;
h q;
h q;
h q;
h q;
h q;
h q;
h q;
h q;
h q;
measure q -> c;
