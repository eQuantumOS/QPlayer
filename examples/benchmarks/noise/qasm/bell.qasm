// Bell state — entanglement; readout + 2q gate error leak |00>/|11> into |01>/|10>.
// 2 qubits: smallest circuit that exercises a coupling-map edge on every device.
OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
creg c[2];
h q[0];
cx q[0],q[1];
measure q[0] -> c[0];
measure q[1] -> c[1];
