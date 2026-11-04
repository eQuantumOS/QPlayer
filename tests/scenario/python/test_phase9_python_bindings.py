"""Phase 9 Python binding tests (Phase 9 row).

Covers what Phase 9 adds to the Python surface:
  * ``QRegister(SimType.TensorNetwork)`` actually runs a circuit ( —
    MPS is the execution representation).
  * ``Result.truncation_errors()`` / ``has_truncation_errors()`` (
) — one entry per 2Q gate on TN, empty elsewhere.
  * ``QRegister.amplitude(basis)``, including past
    MAX_DENSE_EXPORT_QUBITS where it is the only state probe available.
  * ``QRegister.reduced_density_matrix(qubits)`` as a 2-D
    numpy array, and its TensorNetwork-only policy on other backends.
  * ``interactive.initialize()`` rejection on TN.

Uses the stdlib ``unittest`` framework (pytest is not a build/test dependency):

    python -m unittest tests.scenario.python.test_phase9_python_bindings -v
"""

import math
import os
import sys
import unittest

try:
    import qplayer  # noqa: E402
except ModuleNotFoundError:
    _REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    _DEV_PKG = os.path.join(_REPO_ROOT, "bindings")
    if _DEV_PKG not in sys.path:
        sys.path.insert(0, _DEV_PKG)
    import qplayer  # noqa: E402

import numpy as np

_INV_SQRT2 = 1.0 / math.sqrt(2.0)


def _ghz(n):
    circ = qplayer.Circuit(n, n)
    circ.h(0)
    for q in range(n - 1):
        circ.cx(q, q + 1)
    return circ


class TestTensorNetworkRuns(unittest.TestCase):
    def test_tn_bell_distribution(self):
        # TN is a usable backend from on; before Phase 9 this raised.
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4321)
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1)
        counts = qr.run(circ, 4000).counts()
        self.assertAlmostEqual(counts.get("00", 0) / 4000, 0.5, delta=0.04)
        self.assertAlmostEqual(counts.get("11", 0) / 4000, 0.5, delta=0.04)
        self.assertEqual(counts.get("01", 0), 0)
        self.assertEqual(counts.get("10", 0), 0)

    def test_tn_matches_fullstate_statevector(self):
        # Same circuit on both backends, amplitude by amplitude. Distinct 1Q
        # gates per qubit keep the state asymmetric — a symmetric one could not
        # detect a qubit-order bug.
        circ = qplayer.Circuit(4, 4)
        circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).tdg(3)
        tn = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        fs = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        tn.run(circ, 1)
        fs.run(circ, 1)
        np.testing.assert_allclose(tn.get_statevector(), fs.get_statevector(), atol=1e-12)

    def test_tn_interactive_is_rejected(self):
        # : TN has no immediate-mode path.
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        with self.assertRaises(qplayer.SimulatorError):
            qr.interactive.initialize(2)


class TestTruncationErrors(unittest.TestCase):
    def test_tn_reports_one_entry_per_two_qubit_gate(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.run(_ghz(5), 1)          # 4 CX gates
        result = qr.result()
        self.assertTrue(result.has_truncation_errors())
        errors = result.truncation_errors()
        self.assertEqual(len(errors), 4)
        for e in errors:
            self.assertGreaterEqual(e, 0.0)
            self.assertLess(e, 1e-12)   # GHZ needs chi=2, far below the default cap

    def test_fullstate_has_no_truncation_errors(self):
        # The channel is TN-only; other backends leave it empty.
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.run(_ghz(4), 1)
        self.assertFalse(qr.result().has_truncation_errors())
        self.assertEqual(qr.result().truncation_errors(), [])


class TestAmplitude(unittest.TestCase):
    def test_amplitude_matches_statevector_on_every_backend(self):
        circ = qplayer.Circuit(3, 3)
        circ.h(0).t(0).cx(0, 1).cx(1, 2)
        for sim_type in (qplayer.SimType.TensorNetwork,
                         qplayer.SimType.FullState,
                         qplayer.SimType.SparseState):
            qr = (qplayer.QRegister(sim_type, qplayer.ProcType.CPU)
                  if sim_type == qplayer.SimType.FullState
                  else qplayer.QRegister(sim_type))
            qr.run(circ, 1)
            sv = qr.get_statevector()
            for basis in range(8):
                self.assertAlmostEqual(qr.amplitude(basis), sv[basis], delta=1e-12,
                                       msg=f"{sim_type} basis={basis}")

    def test_amplitude_beyond_dense_export_cap(self):
        # 40 qubits: get_statevector() is impossible, so amplitude() is the only
        # way to look at the state at all.
        n = 40
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.run(_ghz(n), 1)
        all_ones = (1 << n) - 1
        self.assertAlmostEqual(abs(qr.amplitude(0)), _INV_SQRT2, delta=1e-12)
        self.assertAlmostEqual(abs(qr.amplitude(all_ones)), _INV_SQRT2, delta=1e-12)
        self.assertAlmostEqual(abs(qr.amplitude(1 << 7)), 0.0, delta=1e-12)
        with self.assertRaises(qplayer.SimulatorError):
            qr.get_statevector()

    def test_amplitude_rejects_bad_index(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.run(_ghz(3), 1)
        with self.assertRaises(qplayer.SimulatorError):
            qr.amplitude(8)          # 2^3 is out of range
        with self.assertRaises(ValueError):
            qr.amplitude(-1)         # negative index is a Python-level error


class TestReducedDensityMatrix(unittest.TestCase):
    def test_bell_half_is_maximally_mixed(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.run(_ghz(2), 1)
        rho = qr.reduced_density_matrix([0])
        self.assertEqual(rho.shape, (2, 2))
        np.testing.assert_allclose(rho, np.array([[0.5, 0.0], [0.0, 0.5]]), atol=1e-12)

    def test_rdm_is_two_dimensional_and_hermitian(self):
        circ = qplayer.Circuit(4, 4)
        circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.run(circ, 1)
        rho = qr.reduced_density_matrix([1, 2])
        self.assertEqual(rho.shape, (4, 4))
        self.assertAlmostEqual(np.trace(rho).real, 1.0, delta=1e-12)
        np.testing.assert_allclose(rho, rho.conj().T, atol=1e-12)

    def test_rdm_is_tensornetwork_only(self):
        # : dense backends point the caller at get_statevector().
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.run(_ghz(3), 1)
        with self.assertRaises(qplayer.SimulatorError):
            qr.reduced_density_matrix([0])


class TestQftOnTensorNetwork(unittest.TestCase):
    def test_qft_4_matches_fullstate(self):
        circ = qplayer.Circuit(4, 4)
        for i in range(4):
            circ.h(i)
            for j in range(i + 1, 4):
                circ.cu1(math.pi / (2 ** (j - i)), j, i)
        tn = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        fs = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        tn.run(circ, 1)
        fs.run(circ, 1)
        np.testing.assert_allclose(tn.get_statevector(), fs.get_statevector(), atol=1e-12)

    def test_qft_50_qubits_is_uniform(self):
        # QFT|0...0> is the uniform superposition, so every amplitude is 2^-25
        # and chi stays at 1 — which is why 50 qubits is feasible at all.
        n = 50
        circ = qplayer.Circuit(n, n)
        for i in range(n):
            circ.h(i)
            for j in range(i + 1, n):
                circ.cu1(math.pi / (2.0 ** (j - i)), j, i)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.run(circ, 1)
        expected = 2.0 ** (-0.5 * n)
        for basis in (0, (1 << n) - 1, 0b1010101):
            self.assertAlmostEqual(abs(qr.amplitude(basis)), expected, delta=1e-15)


if __name__ == "__main__":
    unittest.main(verbosity=2)
