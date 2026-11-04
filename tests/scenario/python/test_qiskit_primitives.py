"""Phase 7-J — primitive auto-provision tests.

QPlayerBackend is a BackendV2, so Qiskit's own BackendSamplerV2 / BackendEstimatorV2
wrap it directly — no dedicated primitive classes. Verifies Bell sampling and a
shot-based <H> estimate.

    python -m unittest tests.python.test_qiskit_primitives -v
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
    from qiskit import QuantumCircuit
    from qiskit.primitives import BackendSamplerV2, BackendEstimatorV2
    from qiskit.quantum_info import SparsePauliOp
    from qplayer.qiskit import QPlayerBackend
    _HAVE_QISKIT = True
except ImportError:
    _HAVE_QISKIT = False


@unittest.skipUnless(_HAVE_QISKIT, "qiskit not installed")
class PrimitivesTest(unittest.TestCase):
    def setUp(self):
        self.backend = QPlayerBackend()

    def test_sampler_v2_bell(self):
        qc = QuantumCircuit(2, 2)
        qc.h(0); qc.cx(0, 1); qc.measure([0, 1], [0, 1])
        sampler = BackendSamplerV2(backend=self.backend)
        sampler.options.seed_simulator = 1
        data = sampler.run([qc], shots=8000).result()[0].data
        counts = data.c.get_counts()
        self.assertEqual(set(counts), {"00", "11"})
        self.assertAlmostEqual(counts["00"] / 8000, 0.5, delta=0.03)

    def test_estimator_v2_weighted_hamiltonian(self):
        # H = ZI + IZ + 0.5 XX on Ry(pi/3) x Ry(pi/3): analytic 0.5+0.5+0.375=1.375
        ansatz = QuantumCircuit(2)
        ansatz.ry(math.pi / 3, 0); ansatz.ry(math.pi / 3, 1)
        H = SparsePauliOp.from_list([("ZI", 1.0), ("IZ", 1.0), ("XX", 0.5)])
        est = BackendEstimatorV2(backend=self.backend)
        est.options.seed_simulator = 1
        ev = est.run([(ansatz, H)], precision=0.005).result()[0].data.evs
        self.assertAlmostEqual(float(ev), 1.375, delta=0.05)  # shot-based estimate


if __name__ == "__main__":
    unittest.main(verbosity=2)
