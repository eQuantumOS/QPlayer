"""Python surface of per-site noise interventions and the draw ledger.

Uses the stdlib ``unittest`` framework (pytest is not a build/test dependency):

    python -m unittest tests.scenario.python.test_noise_intervention -v
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


def _register(sim, sites, ledger):
    qr = qplayer.QRegister(sim, qplayer.ProcType.CPU)
    qr.set_seed(11)
    nm = qplayer.NoiseModel()
    nm.add_bit_flip("id", 1.0)
    qr.set_noise_model(nm)
    qr.set_noise_interventions(sites)
    qr.set_noise_draw_ledger(ledger)
    return qr


def _circuit():
    c = qplayer.Circuit(1, 1)
    c.id(0)
    c.measure(0, 0)
    return c


class NoiseInterventionPython(unittest.TestCase):

    def test_intervention_removes_a_certain_flip(self):
        for sim in (qplayer.SimType.FullState, qplayer.SimType.TensorNetwork):
            counts = _register(sim, [0], False).run(_circuit(), 20, False).counts()
            self.assertEqual(counts, {"0": 20})

    def test_ledger_records_site_op_branch_u(self):
        r = _register(qplayer.SimType.FullState, [], True).run(_circuit(), 3, False)
        ledger = r.noise_draw_ledger()
        self.assertEqual(len(ledger), 3)
        e = ledger[0][0]
        self.assertIsInstance(e, qplayer.NoiseDrawEvent)
        # bit_flip(1.0) is {sqrt(0) I, sqrt(1) X}, so the X branch (1) is taken.
        self.assertEqual((e.site, e.op_index, e.branch, e.intervened), (0, 0, 1, False))
        self.assertTrue(0.0 <= e.u < 1.0)

    def test_list_is_sorted_unique_and_echoed(self):
        qr = _register(qplayer.SimType.FullState, [0, 0], False)
        self.assertEqual(qr.noise_interventions(), [0])
        self.assertEqual(qr.run(_circuit(), 2, False).noise_interventions(), [0])

    def test_site_out_of_range_raises(self):
        qr = _register(qplayer.SimType.FullState, [7], False)
        with self.assertRaises(qplayer.SimulatorError):
            qr.run(_circuit(), 2, False)


if __name__ == "__main__":
    unittest.main()
