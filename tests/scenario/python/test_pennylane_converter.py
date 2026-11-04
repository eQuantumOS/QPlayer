"""Phase 7-K — PennyLane tape -> qplayer.Circuit converter tests.

stdlib unittest (Python-only, not a ctest target). Skips when pennylane absent.

    python -m unittest tests.python.test_pennylane_converter -v
"""
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
    import numpy as np
    import pennylane as qml
    from qplayer.pennylane.converter import (ConversionError, obs_to_pauli_terms,
                                             supported, to_qplayer_circuit)
    _HAVE_PL = True
except ImportError:
    _HAVE_PL = False


@unittest.skipUnless(_HAVE_PL, "pennylane not installed")
class ConverterTest(unittest.TestCase):
    def _tape(self, fn, wires):
        return qml.tape.make_qscript(fn)(), {w: w for w in range(wires)}

    def test_bell_builds_circuit(self):
        tape, w2q = self._tape(lambda: (qml.Hadamard(0), qml.CNOT([0, 1])), 2)
        circ = to_qplayer_circuit(tape, w2q, 2, with_measure=False)
        self.assertEqual(circ.num_qubits(), 2)
        self.assertGreater(circ.num_ops(), 0)

    def test_supported_stopping_condition(self):
        self.assertTrue(supported(qml.Hadamard(0)))
        self.assertTrue(supported(qml.CNOT([0, 1])))
        self.assertFalse(supported(qml.IsingZZ(0.3, wires=[0, 1])))  # must decompose

    def test_pauli_term_mapping_big_endian(self):
        # Z(0) -> qubit0 lowest -> big-endian string "IZ" for 2 qubits.
        obs = qml.Z(0) + qml.Z(1) + 0.5 * qml.X(0) @ qml.X(1)
        terms = dict(obs_to_pauli_terms(obs, {0: 0, 1: 1}, 2))
        self.assertAlmostEqual(terms["IZ"], 1.0, places=12)   # Z on qubit 0
        self.assertAlmostEqual(terms["ZI"], 1.0, places=12)   # Z on qubit 1
        self.assertAlmostEqual(terms["XX"], 0.5, places=12)

    def test_unsupported_op_raises(self):
        tape, w2q = self._tape(lambda: qml.IsingZZ(0.3, wires=[0, 1]), 2)
        with self.assertRaises(ConversionError):
            to_qplayer_circuit(tape, w2q, 2, with_measure=False)  # not decomposed

    def test_measure_appended(self):
        tape, w2q = self._tape(lambda: qml.Hadamard(0), 1)
        circ = to_qplayer_circuit(tape, w2q, 1, with_measure=True)
        self.assertEqual(circ.total_cubits(), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
