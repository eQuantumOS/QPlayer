"""Phase 7-J — plugin packaging / discovery tests.

Covers the public import surface and the ``qiskit.providers`` entry point. The
entry-point resolution test is skipped unless the package was installed with the
extra (``pip install .[qiskit]``) so a dev tree without reinstall still passes.

    python -m unittest tests.scenario.python.test_qiskit_plugin -v
"""
import importlib.metadata as md
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
    from qiskit.providers import BackendV2
    from qplayer.qiskit import (QPlayerBackend, QPlayerJob, QPlayerProvider,
                                to_qplayer_circuit)
    _HAVE_QISKIT = True
except ImportError:
    _HAVE_QISKIT = False


@unittest.skipUnless(_HAVE_QISKIT, "qiskit not installed")
class PluginSurfaceTest(unittest.TestCase):
    def test_public_exports(self):
        self.assertTrue(callable(QPlayerProvider))
        self.assertTrue(callable(QPlayerBackend))
        self.assertTrue(callable(QPlayerJob))
        self.assertTrue(callable(to_qplayer_circuit))

    def test_provider_yields_backendv2(self):
        be = QPlayerProvider().get_backend()
        self.assertIsInstance(be, BackendV2)
        self.assertEqual(be.name, "qplayer")

    def test_provider_proc_option(self):
        # proc="gpu" must not raise on a CPU host (QPlayer falls back w/ warning).
        be = QPlayerProvider().get_backend(proc="gpu")
        self.assertIsInstance(be, BackendV2)

    def test_core_import_does_not_load_qiskit_subpackage(self):
        # Importing the core package must not eagerly pull in qplayer.qiskit.
        # (qplayer.qiskit is only loaded on explicit import —.)
        self.assertNotIn("qplayer.qiskit", _fresh_core_modules())

    def test_entry_point_registered_if_installed(self):
        eps = md.entry_points(group="qiskit.providers")
        names = {ep.name: ep for ep in eps}
        if "qplayer" not in names:
            self.skipTest("entry point not registered (needs pip install .[qiskit])")
        self.assertEqual(names["qplayer"].load(), QPlayerProvider)


def _fresh_core_modules():
    import subprocess
    code = ("import sys, qplayer; "
            "print('qplayer.qiskit' in sys.modules)")
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    return {"qplayer.qiskit"} if out.stdout.strip() == "True" else set()


if __name__ == "__main__":
    unittest.main(verbosity=2)
