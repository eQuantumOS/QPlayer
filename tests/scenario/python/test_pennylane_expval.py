"""Phase 7-K — expval / var / gradient via the exact Observable path.

expval/var map the observable to weighted Pauli strings and evaluate exactly on
the statevector (qplayer Observable Sum(c_i <P_i>) semantics).

    python -m unittest tests.scenario.python.test_pennylane_expval -v
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
    import numpy as np
    import pennylane as qml
    from qplayer.pennylane import QPlayerDevice
    _HAVE_PL = True
except ImportError:
    _HAVE_PL = False


@unittest.skipUnless(_HAVE_PL, "pennylane not installed")
class ExpvalTest(unittest.TestCase):
    def test_weighted_hamiltonian_analytic(self):
        # H = Z0 + Z1 + 0.5 X0X1 on Ry(pi/3) x Ry(pi/3): 0.5+0.5+0.5*0.75 = 1.375.
        dev = QPlayerDevice(wires=2)
        @qml.qnode(dev)
        def f():
            qml.RY(math.pi / 3, 0); qml.RY(math.pi / 3, 1)
            return qml.expval(qml.Z(0) + qml.Z(1) + 0.5 * qml.X(0) @ qml.X(1))
        self.assertAlmostEqual(float(f()), 1.375, places=10)

    def test_expval_matches_default(self):
        def f():
            qml.RY(0.9, 0); qml.RX(0.4, 1); qml.CNOT([0, 1])
            return qml.expval(qml.Z(0) @ qml.Z(1) + 0.3 * qml.X(0))
        q = qml.QNode(f, QPlayerDevice(wires=2))()
        d = qml.QNode(f, qml.device("default.qubit", wires=2))()
        self.assertAlmostEqual(float(q), float(d), places=9)

    def test_variance_matches_default(self):
        def f():
            qml.RY(0.9, 0)
            return qml.var(qml.Z(0))
        q = qml.QNode(f, QPlayerDevice(wires=1))()
        d = qml.QNode(f, qml.device("default.qubit", wires=1))()
        self.assertAlmostEqual(float(q), float(d), places=9)

    def test_parameter_shift_gradient(self):
        dev = QPlayerDevice(wires=1)
        @qml.qnode(dev, diff_method="parameter-shift")
        def cost(x):
            qml.RY(x, 0)
            return qml.expval(qml.Z(0))
        x = qml.numpy.array(0.5, requires_grad=True)
        grad = qml.grad(cost)(x)
        self.assertAlmostEqual(float(grad), -math.sin(0.5), places=6)  # d/dx cos(x)


if __name__ == "__main__":
    unittest.main(verbosity=2)
