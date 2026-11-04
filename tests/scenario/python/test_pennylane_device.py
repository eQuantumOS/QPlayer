"""Phase 7-K — QPlayerDevice measurement tests.

Validates state/probs/sample/counts against PennyLane's default.qubit reference.

    python -m unittest tests.python.test_pennylane_device -v
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
    from pennylane.devices import Device
    from qplayer.pennylane import QPlayerDevice
    _HAVE_PL = True
except ImportError:
    _HAVE_PL = False


@unittest.skipUnless(_HAVE_PL, "pennylane not installed")
class DeviceTest(unittest.TestCase):
    def _both(self, fn, wires=2, shots=None):
        q = QPlayerDevice(wires=wires, shots=shots)
        d = qml.device("default.qubit", wires=wires, shots=shots)
        return qml.QNode(fn, q)(), qml.QNode(fn, d)()

    def test_is_device_subclass(self):
        self.assertIsInstance(QPlayerDevice(wires=2), Device)
        self.assertEqual(QPlayerDevice(wires=2).name, "qplayer.qubit")

    def test_probs_full_matches_default(self):
        def f():
            qml.RY(0.7, 0); qml.RX(0.3, 1); qml.CNOT([0, 1])
            return qml.probs(wires=[0, 1])
        a, b = self._both(f)
        np.testing.assert_allclose(np.asarray(a), np.asarray(b), atol=1e-9)

    def test_probs_marginal_matches_default(self):
        def f():
            qml.RY(0.7, 0); qml.RX(0.3, 1); qml.CNOT([0, 1])
            return qml.probs(wires=[1])
        a, b = self._both(f)
        np.testing.assert_allclose(np.asarray(a), np.asarray(b), atol=1e-9)

    def test_state_matches_default(self):
        def f():
            qml.Hadamard(0); qml.CNOT([0, 1])
            return qml.state()
        a, b = self._both(f)
        np.testing.assert_allclose(np.asarray(a), np.asarray(b), atol=1e-9)

    def test_counts_bell(self):
        dev = QPlayerDevice(wires=2, shots=8000, seed=1)
        @qml.qnode(dev)
        def f():
            qml.Hadamard(0); qml.CNOT([0, 1])
            return qml.counts()
        counts = f()
        self.assertEqual(set(counts), {"00", "11"})
        self.assertAlmostEqual(int(counts["00"]) / 8000, 0.5, delta=0.03)

    def test_sample_shape(self):
        dev = QPlayerDevice(wires=2, shots=500, seed=1)
        @qml.qnode(dev)
        def f():
            qml.Hadamard(0); qml.CNOT([0, 1])
            return qml.sample()
        self.assertEqual(np.asarray(f()).shape, (500, 2))

    def test_decompose_unsupported_gate(self):
        # IsingZZ / Rot are not in the supported set -> preprocess must decompose.
        def f():
            qml.Rot(0.2, 0.4, 0.6, wires=0); qml.IsingZZ(0.5, wires=[0, 1])
            qml.CNOT([0, 1])
            return qml.probs(wires=[0, 1])
        a, b = self._both(f)
        np.testing.assert_allclose(np.asarray(a), np.asarray(b), atol=1e-9)


if __name__ == "__main__":
    unittest.main(verbosity=2)
