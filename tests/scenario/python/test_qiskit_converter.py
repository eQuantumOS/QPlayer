"""Phase 7-J — QuantumCircuit -> qplayer.Circuit converter tests.

stdlib unittest (Python-only, not a ctest target). Skips entirely when
the optional qiskit dependency is absent.

    python -m unittest tests.scenario.python.test_qiskit_converter -v
"""
import math
import os
import sys
import unittest

try:
    import qplayer  # noqa: F401
except ModuleNotFoundError:
    _REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    sys.path.insert(0, os.path.join(_REPO, "bindings"))
    import qplayer  # noqa: F401

try:
    from qiskit import QuantumCircuit, transpile
    from qiskit.quantum_info import Statevector
    from qplayer.qiskit import QPlayerBackend
    from qplayer.qiskit.converter import to_qplayer_circuit, ConversionError
    _HAVE_QISKIT = True
except ImportError:
    _HAVE_QISKIT = False


def _state_equiv(qc):
    """True if the converted QPlayer circuit yields the same statevector as Qiskit
    (up to global phase), measured by overlap |<psi_qk|psi_qp>| ~ 1."""
    import numpy as np
    qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
    circ = to_qplayer_circuit(qc)
    qr.run(circ, 1)
    psi_qp = qr.get_statevector()
    psi_qk = np.asarray(Statevector(qc.remove_final_measurements(inplace=False)))
    return abs(np.vdot(psi_qk, psi_qp))


@unittest.skipUnless(_HAVE_QISKIT, "qiskit not installed")
class ConverterTest(unittest.TestCase):
    def _transpile(self, qc):
        return transpile(qc, backend=QPlayerBackend(), seed_transpiler=1)

    def test_bell_maps_and_runs(self):
        qc = QuantumCircuit(2, 2)
        qc.h(0); qc.cx(0, 1); qc.measure([0, 1], [0, 1])
        circ = to_qplayer_circuit(self._transpile(qc))
        self.assertEqual(circ.num_qubits(), 2)
        self.assertGreater(circ.num_ops(), 0)

    def test_ghz_statevector_equivalent(self):
        qc = QuantumCircuit(4)
        qc.h(0); qc.cx(0, 1); qc.cx(1, 2); qc.cx(2, 3)
        self.assertAlmostEqual(_state_equiv(self._transpile(qc)), 1.0, places=9)

    def test_rotation_gates_statevector_equivalent(self):
        qc = QuantumCircuit(2)
        qc.ry(math.pi / 3, 0); qc.rz(math.pi / 5, 0); qc.rx(math.pi / 7, 1)
        qc.crz(math.pi / 4, 0, 1)
        self.assertAlmostEqual(_state_equiv(self._transpile(qc)), 1.0, places=9)

    def test_measure_mapping_preserves_clbit_index(self):
        # x on q0,q2 -> classical bits c0,c2 set; key char order = c2 c1 c0.
        qc = QuantumCircuit(3, 3)
        qc.x(0); qc.x(2); qc.measure(range(3), range(3))
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.set_seed(1)
        counts = dict(qr.run(to_qplayer_circuit(self._transpile(qc)), 100).counts())
        self.assertEqual(counts, {"101": 100})

    def test_unsupported_op_raises(self):
        # An un-transpiled exotic op (e.g. a 4-controlled gate) must raise clearly.
        qc = QuantumCircuit(2)
        qc.rzz(0.3, 0, 1)  # not in QPlayer's set; converting raw must fail
        with self.assertRaises(ConversionError):
            to_qplayer_circuit(qc)


if __name__ == "__main__":
    unittest.main(verbosity=2)
