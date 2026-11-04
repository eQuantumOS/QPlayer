"""Phase 7 Python binding unit tests.

Covers the Batch API surface exposed in Phase 7: Bell / GHZ / QFT circuits over
the FullState CPU and SparseState backends, numpy statevector access, and the
four Python exception types (QASMParseError / CircuitError / SimulatorError /
NotImplementedError). Noise / Interactive / TensorNetwork are out of scope until
Phase 8+.

Uses the stdlib `unittest` framework (pytest is not a build/test dependency).
After installing the package (`pip install .` from the extracted source), run:

    python -m unittest tests.python.test_phase7_python_bindings -v

or simply `python tests/scenario/python/test_phase7_python_bindings.py`.
"""

import math
import os
import sys
import unittest

# Prefer an installed `qplayer` package (the normal path: `pip install .`).
# Only fall back to the in-tree source layout (bindings/qplayer/, parent dir bindings/) when the package
# is not installed -- that directory has no compiled `_qplayer` extension unless
# it was built there, so it must never shadow a real install.
try:
    import qplayer  # noqa: E402
except ModuleNotFoundError:
    _REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    _DEV_PKG = os.path.join(_REPO_ROOT, "bindings")
    if _DEV_PKG not in sys.path:
        sys.path.insert(0, _DEV_PKG)
    import qplayer  # noqa: E402

_NSHOTS = 10000
_TOL = 0.02  # statistical tolerance at nshots=10000


class TestBellState(unittest.TestCase):
    def _bell_distribution(self, sim_type):
        qr = qplayer.QRegister(sim_type)
        qr.set_seed(1234)
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1)
        counts = qr.run(circ, _NSHOTS).counts()
        p00 = counts.get("00", 0) / _NSHOTS
        p11 = counts.get("11", 0) / _NSHOTS
        self.assertAlmostEqual(p00, 0.5, delta=_TOL)
        self.assertAlmostEqual(p11, 0.5, delta=_TOL)
        # No "01"/"10" — the qubits are maximally entangled.
        self.assertEqual(counts.get("01", 0), 0)
        self.assertEqual(counts.get("10", 0), 0)

    def test_bell_fullstate(self):
        self._bell_distribution(qplayer.SimType.FullState)

    def test_bell_sparsestate(self):
        self._bell_distribution(qplayer.SimType.SparseState)


class TestGHZState(unittest.TestCase):
    def test_ghz3_fullstate(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        qr.set_seed(2024)
        circ = qplayer.Circuit(3, 3)
        circ.h(0).cx(0, 1).cx(0, 2).measure_all()
        counts = qr.run(circ, _NSHOTS).counts()
        p000 = counts.get("000", 0) / _NSHOTS
        p111 = counts.get("111", 0) / _NSHOTS
        self.assertAlmostEqual(p000, 0.5, delta=_TOL)
        self.assertAlmostEqual(p111, 0.5, delta=_TOL)
        # Only the two GHZ outcomes appear.
        self.assertEqual(sum(counts.values()), _NSHOTS)
        self.assertEqual(p000 + p111, 1.0)


class TestQFT(unittest.TestCase):
    def test_qft3_uniform_statevector(self):
        n = 3
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        circ = qplayer.Circuit(n, n)
        for j in range(n):
            circ.h(j)
            for k in range(j + 1, n):
                circ.cu1(math.pi / (2 ** (k - j)), k, j)
        for i in range(n // 2):
            circ.swap(i, n - 1 - i)

        sv = qr.run(circ, 1).statevector()
        dim = 2 ** n
        self.assertEqual(len(sv), dim)
        # QFT|0...0> is the uniform superposition: every |amp|^2 == 1/N.
        for amp in sv:
            self.assertAlmostEqual(abs(amp) ** 2, 1.0 / dim, delta=1e-9)


class TestStatevectorNumpy(unittest.TestCase):
    def test_statevector_is_complex128(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        circ = qplayer.Circuit(1, 1)
        circ.h(0)
        sv = qr.run(circ, 1).statevector()
        # AoS numpy array, dtype complex128 (-SA-03).
        self.assertEqual(str(sv.dtype), "complex128")
        self.assertEqual(sv.shape, (2,))
        # H|0> = (|0> + |1>)/sqrt(2): both amplitudes ~ 1/sqrt(2).
        self.assertAlmostEqual(abs(sv[0]), 1.0 / math.sqrt(2), delta=1e-9)
        self.assertAlmostEqual(abs(sv[1]), 1.0 / math.sqrt(2), delta=1e-9)


class TestExceptions(unittest.TestCase):
    def test_qasm_parse_error(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        with self.assertRaises(qplayer.QASMParseError):
            qr.parse("this is not valid OpenQASM at all ;;;")

    def test_circuit_error_out_of_range_qubit(self):
        circ = qplayer.Circuit(2, 2)
        with self.assertRaises(qplayer.CircuitError):
            circ.h(5)  # qubit index >= num_qubits

    def test_simulator_error_sparse_raw_view(self):
        qr = qplayer.QRegister(qplayer.SimType.SparseState)
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1)
        qr.run(circ, 1)
        # SparseState has no SoA buffer.
        with self.assertRaises(qplayer.SimulatorError):
            qr.get_statevector_raw()

    def test_not_implemented_error_sparse_expectation(self):
        qr = qplayer.QRegister(qplayer.SimType.SparseState)
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1)
        qr.run(circ, 1)
        # expectation_value deferred on SparseState (Phase 7 T7.x scope note).
        with self.assertRaises(qplayer.NotImplementedError):
            qr.expectation_value("ZZ")

    def test_exception_hierarchy_parent_catch(self):
        # Every leaf is catchable as the QPlayerError base.
        circ = qplayer.Circuit(2, 2)
        with self.assertRaises(qplayer.QPlayerError):
            circ.h(5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
