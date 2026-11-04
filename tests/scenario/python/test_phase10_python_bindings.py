"""Phase 10 Python binding tests (Phase 10 row).

Covers what Phase 10 adds to the Python surface:
  * TensorNetwork under a ``NoiseModel`` — the same object handed to FullState
    and to TN produces the same distribution.
  * Route selection ``QRegister.set_tn_noise_path("mc"|"mpo")``, the
    knobs ``set_mps_truncation`` / ``set_mpo_truncation``, and their behaviour on
    a backend that cannot honour them.
  * Noisy counts past the dense export cap — a 100-qubit noisy circuit produces
    counts from Python, which native MPS sampling is what makes
    possible; FullState would need 2**100 amplitudes.
  * ``Result.truncation_errors()`` as the accuracy axis of a capped run, and
    ``Result.expectation()`` under TN noise.

Uses the stdlib ``unittest`` framework (pytest is not a build/test dependency):

    python -m unittest tests.scenario.python.test_phase10_python_bindings -v
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


def _tvd(a, b, shots):
    """Total variation distance between two count dicts, as probabilities."""
    keys = set(a) | set(b)
    return 0.5 * sum(abs(a.get(k, 0) / shots - b.get(k, 0) / shots) for k in keys)


def _depol_ghz(n, p=0.02):
    circ = qplayer.Circuit(n, n)
    circ.h(0)
    for q in range(n - 1):
        circ.cx(q, q + 1)
    circ.measure_all()
    noise = qplayer.NoiseModel()
    noise.add_depolarizing("cx", p)
    return circ, noise


class TestTensorNetworkNoise(unittest.TestCase):
    def test_tn_noise_matches_fullstate(self):
        # The claim Phase 10 rests on: one NoiseModel object, two backends, same
        # distribution. Anything weaker (TN runs without raising) would not show it.
        circ, noise = _depol_ghz(4)
        shots = 20000
        counts = {}
        for sim in (qplayer.SimType.FullState, qplayer.SimType.TensorNetwork):
            qr = qplayer.QRegister(sim)
            qr.set_seed(20260826)
            qr.set_noise_model(noise)
            counts[sim] = qr.run(circ, shots).counts()
        self.assertLess(
            _tvd(counts[qplayer.SimType.FullState],
                 counts[qplayer.SimType.TensorNetwork], shots),
            0.02,
        )

    def test_device_model_noise_matches_fullstate(self):
        # load_device_model() synthesises relaxation + depolarizing + readout at
        # once, so this exercises the whole stack rather than one
        # hand-attached channel. Skipped rather than failed when the benchmark
        # device JSON is absent from an installed wheel.
        repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        device = os.path.join(repo, "tests", "data", "device_json", "willow_like.json")
        if not os.path.exists(device):
            self.skipTest("device JSON not available in this tree")
        circ = qplayer.Circuit(3, 3)
        circ.h(0).cx(0, 1).cx(1, 2).measure_all()
        shots = 20000
        counts = {}
        for sim in (qplayer.SimType.FullState, qplayer.SimType.TensorNetwork):
            qr = qplayer.QRegister(sim)
            qr.set_seed(31415)
            qr.load_device_model(device)
            counts[sim] = qr.run(circ, shots).counts()
        self.assertLess(
            _tvd(counts[qplayer.SimType.FullState],
                 counts[qplayer.SimType.TensorNetwork], shots),
            0.02,
        )

    def test_expectation_under_tn_noise(self):
        # Before the expectation dict came back empty on the noisy TN path.
        # ZZ on a depolarised Bell pair is below 1 but well above 0, so a value of
        # exactly 0.0 (an unfilled dict) or 1.0 (noise not applied) both fail.
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1)
        circ.add_expectation("ZZ")
        noise = qplayer.NoiseModel()
        noise.add_depolarizing("cx", 0.05)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(99)
        qr.set_noise_model(noise)
        result = qr.run(circ, 4000)
        zz = result.expectation("ZZ")
        self.assertGreater(zz, 0.5)
        self.assertLess(zz, 0.999)


class TestNoiseRouteSelection(unittest.TestCase):
    def test_mpo_route_agrees_with_monte_carlo(self):
        # Route selection is the user choice. Both routes carry the same
        # physics by different means, so agreeing is the check that the string
        # reached the backend and that the density matrix path is not a different
        # model. Same process, same circuit — which is what env-only selection
        # could not do.
        circ, noise = _depol_ghz(4)
        shots = 20000
        out = {}
        for route in ("mc", "mpo"):
            qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
            qr.set_seed(7)
            qr.set_noise_model(noise)
            qr.set_tn_noise_path(route)
            out[route] = qr.run(circ, shots).counts()
        self.assertLess(_tvd(out["mc"], out["mpo"], shots), 0.02)

    def test_unknown_route_raises(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        for bad in ("mpa", "", "MPO", "monte-carlo"):
            with self.assertRaises(qplayer.CircuitError):
                qr.set_tn_noise_path(bad)

    def test_truncation_knobs_change_the_reported_error(self):
        # The knob has no getter, so its effect is the observable: a tight cap has
        # to discard weight where a generous one does not.
        circ = qplayer.Circuit(12, 12)
        for layer in range(8):
            for q in range(layer % 2, 11, 2):
                circ.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1)
        circ.measure_all()
        noise = qplayer.NoiseModel()
        noise.add_depolarizing("cx", 0.01)

        errors = {}
        for cap in (4, 4096):
            qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
            qr.set_seed(11)
            qr.set_noise_model(noise)
            qr.set_mps_truncation(cap, 1e-12)
            result = qr.run(circ, 4)
            errors[cap] = sum(e * e for e in result.truncation_errors()) ** 0.5
        self.assertGreater(errors[4], 0.1)
        self.assertAlmostEqual(errors[4096], 0.0, places=12)

    def test_mpo_truncation_rejects_unhonourable_values(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        with self.assertRaises(qplayer.CircuitError):
            qr.set_mpo_truncation(0, 1e-12)
        with self.assertRaises(qplayer.CircuitError):
            qr.set_mps_truncation(8, -1.0)

    def test_knobs_on_fullstate_are_ignored_not_fatal(self):
        # A script that sweeps backends reuses one setup function, so these warn
        # instead of raising — and the run must be unaffected.
        qr = qplayer.QRegister(qplayer.SimType.FullState)
        qr.set_seed(5)
        qr.set_tn_noise_path("mpo")
        qr.set_mps_truncation(4, 1e-12)
        qr.set_mpo_truncation(4, 1e-12)
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1).measure_all()
        result = qr.run(circ, 4000)
        self.assertAlmostEqual(result.counts().get("00", 0) / 4000, 0.5, delta=0.04)
        self.assertFalse(result.has_truncation_errors())


class TestNoisyCountsBeyondDenseCap(unittest.TestCase):
    def test_hundred_qubit_noisy_ghz_counts(self):
        # The Phase 10 headline from Python: 100 qubits under noise, with counts.
        # FullState would need 2**100 amplitudes (~1e19 TB); native MPS sampling
        # draws each shot in O(n * chi^2) instead. GHZ keeps chi at 2,
        # so weak noise leaves the two branches dominant and the distribution has
        # a checkable shape rather than only a shot total.
        n = 100
        circ, noise = _depol_ghz(n, p=0.002)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4321)
        qr.set_noise_model(noise)
        counts = qr.run(circ, 200).counts()
        self.assertEqual(sum(counts.values()), 200)
        for key in counts:
            self.assertEqual(len(key), n)
        # Both clean GHZ branches survive weak noise and stay the modes.
        self.assertIn("0" * n, counts)
        self.assertIn("1" * n, counts)
        self.assertGreater(counts["0" * n] + counts["1" * n], 100)

    def test_capped_hundred_qubit_run_reports_its_truncation(self):
        # Past the dense cap there is no reference distribution, so the reported
        # error is the only judgement available (4). A brickwork
        # at chi_max 16 must come back WITH that channel non-empty — silence here
        # would look exactly like a clean run. Depth 12 is deliberate: depth 8
        # reaches exactly chi = 16 and truncates nothing, so the test would pass
        # for the wrong reason (the n=100 table has both rows).
        n = 100
        circ = qplayer.Circuit(n, n)
        for layer in range(12):
            for q in range(layer % 2, n - 1, 2):
                circ.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1)
        circ.measure_all()
        noise = qplayer.NoiseModel()
        noise.add_depolarizing("cx", 0.01)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(8080)
        qr.set_noise_model(noise)
        qr.set_mps_truncation(16, 1e-12)
        result = qr.run(circ, 4)
        self.assertEqual(sum(result.counts().values()), 4)
        self.assertTrue(result.has_truncation_errors())
        total = sum(e * e for e in result.truncation_errors()) ** 0.5
        self.assertGreater(total, 0.1)


class TrajectoryDiagnostics(unittest.TestCase):
    """Result.bond_dimensions() / norm_drifts().

    Indexed by SHOT, unlike truncation_errors(), which is the worst trajectory's
    per-STEP history. The value is the PEAK bond over the run, not its end-of-run
    value -- the two differ by 2x on a surface code, so a test that only checked
    "some positive number" would pass on the wrong one.

    Only the paths that hold one chain PER SHOT record anything: the noise
    trajectories, and a circuit with mid-circuit measurement / reset. The
    noiseless single-chain fast path reports nothing, which is a contract and not
    an omission -- one chain served every shot there.
    """

    @staticmethod
    def _midcircuit(n=3):
        # A mid-circuit measurement + reset forces the per-shot path: each shot
        # collapses differently, so each shot really does have its own chain.
        circ = qplayer.Circuit(n, n + 1)
        circ.h(0)
        for q in range(n - 1):
            circ.cx(q, q + 1)
        circ.measure(0, n)
        circ.reset(0)
        for q in range(n):
            circ.measure(q, q)
        return circ

    @staticmethod
    def _depol_noise(p=0.02):
        noise = qplayer.NoiseModel()
        noise.add_depolarizing("h", p)
        noise.add_depolarizing("cx", p)
        return noise

    def test_recorded_once_per_shot_on_the_noise_path(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4242)
        qr.set_noise_model(self._depol_noise())
        result = qr.run(self._midcircuit(), 6)
        self.assertTrue(result.has_trajectory_diagnostics())
        self.assertEqual(len(result.bond_dimensions()), 6)
        self.assertEqual(len(result.norm_drifts()), 6)

    def test_recorded_on_the_noiseless_per_shot_path(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4242)
        result = qr.run(self._midcircuit(), 5)
        self.assertTrue(result.has_trajectory_diagnostics())
        self.assertEqual(len(result.bond_dimensions()), 5)

    def test_peak_bond_survives_a_bond_that_shrinks_back(self):
        # H, CX, CX entangles the pair and then undoes it, so the chain ends as a
        # product state. A diagnostic reading the end of the run would report 1;
        # the peak is 2. The trailing measure + reset is what routes this to the
        # per-shot path at all.
        #
        # The barrier is load-bearing: without it TN gate fusion folds
        # the two CX into one identity step, the pair is never entangled, and the
        # peak really is 1. That is correct behaviour, but it would make this test
        # pass for the wrong reason.
        circ = qplayer.Circuit(2, 3)
        circ.h(0)
        circ.cx(0, 1)
        circ.barrier()
        circ.cx(0, 1)
        circ.measure(0, 2)
        circ.reset(0)
        circ.measure(0, 0)
        circ.measure(1, 1)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4242)
        result = qr.run(circ, 4)
        self.assertEqual(max(result.bond_dimensions()), 2)

    def test_noiseless_run_does_not_drift_the_norm(self):
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4242)
        result = qr.run(self._midcircuit(), 4)
        self.assertLess(max(result.norm_drifts()), 1e-9)

    def test_absent_on_the_noiseless_single_chain_fast_path(self):
        # No mid-circuit measurement and no noise: one chain serves every shot,
        # so there is no per-shot history to report.
        circ = qplayer.Circuit(3, 3)
        circ.h(0)
        circ.cx(0, 1)
        circ.cx(1, 2)
        for q in range(3):
            circ.measure(q, q)
        qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
        qr.set_seed(4242)
        result = qr.run(circ, 4)
        self.assertFalse(result.has_trajectory_diagnostics())
        self.assertEqual(result.bond_dimensions(), [])

    def test_absent_on_full_state(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.set_seed(4242)
        result = qr.run(self._midcircuit(), 4)
        self.assertFalse(result.has_trajectory_diagnostics())
        self.assertEqual(result.bond_dimensions(), [])
        self.assertEqual(result.norm_drifts(), [])

    def test_repeat_run_reproduces_the_record(self):
        # the shot index derives the seed, so the same seed
        # gives the same per-shot record element by element.
        circ = self._midcircuit()
        out = []
        for _ in range(2):
            qr = qplayer.QRegister(qplayer.SimType.TensorNetwork)
            qr.set_seed(4242)
            out.append(list(qr.run(circ, 8).bond_dimensions()))
        self.assertEqual(out[0], out[1])


if __name__ == "__main__":
    unittest.main()
