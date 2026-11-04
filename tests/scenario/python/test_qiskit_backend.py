"""Phase 7-J — QPlayerBackend(BackendV2) + Job tests.

Verifies transpile(qc, backend=...) + backend.run().result() and that the result
counts bit order matches Qiskit's convention (compared against Aer when present).

    python -m unittest tests.scenario.python.test_qiskit_backend -v
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
    from qiskit import QuantumCircuit, transpile
    from qiskit.providers import BackendV2, JobStatus
    from qplayer.qiskit import QPlayerBackend, QPlayerProvider
    _HAVE_QISKIT = True
except ImportError:
    _HAVE_QISKIT = False

try:
    from qiskit_aer import AerSimulator
    _HAVE_AER = True
except ImportError:
    _HAVE_AER = False


@unittest.skipUnless(_HAVE_QISKIT, "qiskit not installed")
class BackendTest(unittest.TestCase):
    def setUp(self):
        self.backend = QPlayerProvider().get_backend()

    def test_is_backendv2_with_target(self):
        self.assertIsInstance(self.backend, BackendV2)
        self.assertEqual(self.backend.max_circuits, 1)
        names = set(self.backend.target.operation_names)
        for g in ("cx", "h", "rz", "measure"):
            self.assertIn(g, names)

    def test_transpile_and_run_bell(self):
        qc = QuantumCircuit(2, 2)
        qc.h(0); qc.cx(0, 1); qc.measure([0, 1], [0, 1])
        tqc = transpile(qc, backend=self.backend, seed_transpiler=1)
        job = self.backend.run(tqc, shots=8000, seed_simulator=1)
        self.assertEqual(job.status(), JobStatus.DONE)
        counts = job.result().get_counts()
        self.assertEqual(set(counts), {"00", "11"})
        self.assertAlmostEqual(counts["00"] / 8000, 0.5, delta=0.03)

    def test_seed_reproducible(self):
        qc = QuantumCircuit(2, 2)
        qc.h(0); qc.cx(0, 1); qc.measure([0, 1], [0, 1])
        tqc = transpile(qc, backend=self.backend, seed_transpiler=1)
        a = self.backend.run(tqc, shots=2000, seed_simulator=42).result().get_counts()
        b = self.backend.run(tqc, shots=2000, seed_simulator=42).result().get_counts()
        self.assertEqual(a, b)

    def test_endianness_asymmetric(self):
        # x on q0,q2 -> '101' (c2 c1 c0). Bit order must match Qiskit's.
        qc = QuantumCircuit(3, 3)
        qc.x(0); qc.x(2); qc.measure(range(3), range(3))
        tqc = transpile(qc, backend=self.backend, seed_transpiler=1)
        counts = self.backend.run(tqc, shots=300, seed_simulator=1).result().get_counts()
        self.assertEqual(counts, {"101": 300})

    @unittest.skipUnless(_HAVE_AER, "qiskit-aer not installed")
    def test_counts_match_aer(self):
        qc = QuantumCircuit(3, 3)
        qc.h(0); qc.cx(0, 1); qc.x(2); qc.measure(range(3), range(3))
        tqc = transpile(qc, backend=self.backend, seed_transpiler=1)
        qp_counts = self.backend.run(tqc, shots=20000, seed_simulator=1).result().get_counts()

        sim = AerSimulator(method="statevector")
        atqc = transpile(qc, sim, optimization_level=0, seed_transpiler=1)
        aer_counts = sim.run(atqc, shots=20000, seed_simulator=1).result().get_counts()

        # Same support (the nonzero bitstrings) — endianness/key format identical.
        self.assertEqual(set(qp_counts), set(aer_counts))


if __name__ == "__main__":
    unittest.main(verbosity=2)
