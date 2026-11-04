"""Phase 7-I Python binding tests — Observable / Estimator / Sampler.

Qiskit-compatible primitives over the existing Batch API. Estimator computes
<H> = Sum(c_i*<P_i>) exactly on the statevector; Sampler wraps run().counts().

Uses the stdlib `unittest` framework (pytest is not a build/test dependency):

    python -m unittest tests.scenario.python.test_primitives -v
    python tests/scenario/python/test_primitives.py
"""

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


def _bell_prep():
    # |Phi+> = (|00> + |11>)/sqrt(2), no measurement (state prep for Estimator).
    circ = qplayer.Circuit(2, 2)
    circ.h(0).cx(0, 1)
    return circ


class ObservableTest(unittest.TestCase):
    def test_from_list_preserves_terms(self):
        obs = qplayer.Observable.from_list([("ZZ", 0.5), ("XX", 0.5)])
        self.assertEqual(len(obs), 2)
        # terms() -> list[(coeff, pauli)]
        terms = obs.terms()
        self.assertAlmostEqual(terms[0][0], 0.5, places=12)
        self.assertEqual(terms[0][1], "ZZ")

    def test_add_term_chains(self):
        obs = qplayer.Observable().add_term(1.0, "ZI").add_term(-2.0, "IZ")
        self.assertEqual(len(obs), 2)
        self.assertAlmostEqual(obs.terms()[1][0], -2.0, places=12)
        self.assertEqual(obs.terms()[1][1], "IZ")

    def test_invalid_pauli_raises_circuit_error(self):
        obs = qplayer.Observable()
        with self.assertRaises(qplayer.CircuitError):
            obs.add_term(1.0, "ZQ")


class EstimatorTest(unittest.TestCase):
    def test_bell_weighted_sum_exact(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        est = qplayer.Estimator(qr)
        obs = qplayer.Observable.from_list([("ZZ", 0.5), ("XX", 0.5)])
        self.assertAlmostEqual(est.run(_bell_prep(), obs), 1.0, places=10)

    def test_bell_yy_is_minus_one(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        est = qplayer.Estimator(qr)
        obs = qplayer.Observable.from_list([("YY", 1.0)])
        self.assertAlmostEqual(est.run(_bell_prep(), obs), -1.0, places=10)


class SamplerTest(unittest.TestCase):
    def test_bell_distribution(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        qr.set_seed(42)
        sampler = qplayer.Sampler(qr)

        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1)

        counts = sampler.run(circ, 10000)
        self.assertEqual(sum(counts.values()), 10000)
        self.assertEqual(counts.get("01", 0), 0)
        self.assertEqual(counts.get("10", 0), 0)
        self.assertAlmostEqual(counts.get("00", 0) / 10000.0, 0.5, delta=0.03)
        self.assertAlmostEqual(counts.get("11", 0) / 10000.0, 0.5, delta=0.03)


if __name__ == "__main__":
    unittest.main(verbosity=2)
