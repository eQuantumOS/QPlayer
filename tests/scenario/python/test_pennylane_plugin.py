"""Phase 7-K — plugin packaging / discovery tests.

The entry-point discovery test (qml.device("qplayer.qubit")) is skipped unless the
package was installed with the extra (``pip install .[pennylane]``).

    python -m unittest tests.scenario.python.test_pennylane_plugin -v
"""
import importlib.metadata as md
import os
import subprocess
import sys
import unittest

try:
    import qplayer  # noqa: F401
except ModuleNotFoundError:
    _REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    sys.path.insert(0, os.path.join(_REPO, "bindings"))
    import qplayer  # noqa: F401

try:
    from pennylane.devices import Device
    from qplayer.pennylane import QPlayerDevice
    _HAVE_PL = True
except ImportError:
    _HAVE_PL = False


@unittest.skipUnless(_HAVE_PL, "pennylane not installed")
class PluginSurfaceTest(unittest.TestCase):
    def test_public_export(self):
        self.assertTrue(callable(QPlayerDevice))
        self.assertTrue(issubclass(QPlayerDevice, Device))

    def test_core_import_does_not_load_pennylane_subpackage(self):
        code = "import sys, qplayer; print('qplayer.pennylane' in sys.modules)"
        out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
        self.assertEqual(out.stdout.strip(), "False")

    def test_proc_option(self):
        # proc="gpu" must not raise on a CPU host (QPlayer falls back with a warning).
        dev = QPlayerDevice(wires=1, proc="gpu")
        self.assertIsInstance(dev, Device)

    def test_entry_point_registered_if_installed(self):
        eps = md.entry_points(group="pennylane.plugins")
        names = {ep.name: ep for ep in eps}
        if "qplayer.qubit" not in names:
            self.skipTest("entry point not registered (needs pip install .[pennylane])")
        self.assertEqual(names["qplayer.qubit"].load(), QPlayerDevice)
        import pennylane as qml
        self.assertIsInstance(qml.device("qplayer.qubit", wires=1), QPlayerDevice)


if __name__ == "__main__":
    unittest.main(verbosity=2)
