"""Phase 8 Python binding tests (Phase 8 row).

Covers:
  - Regression: Interactive gate chain still works.
  - NoiseModel: add_depolarizing chaining, set_noise_model, has_channels_for_gate.
  - DeviceModel: from_json_string, T1/T2, coupling_map.
  - ConnectivityError: registered as a qplayer exception subclassing CircuitError.
  - load_device_model: loads a temp file, no exception.
  - Exception hierarchy: ConnectivityError issubclass CircuitError issubclass QPlayerError.

Uses the stdlib `unittest` framework (pytest is not a build/test dependency).
After installing (`pip install .`), run:

    python -m unittest tests.python.test_phase8_python_bindings -v

or simply `python tests/scenario/python/test_phase8_python_bindings.py`.
"""

import math
import os
import sys
import tempfile
import unittest

try:
    import qplayer
except ModuleNotFoundError:
    _REPO_ROOT = os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    _DEV_PKG = os.path.join(_REPO_ROOT, "bindings")
    if _DEV_PKG not in sys.path:
        sys.path.insert(0, _DEV_PKG)
    import qplayer

# Minimal device JSON with 2 qubits, one gate each, and a linear coupling map.
_DEVICE_JSON = """{
  "qubits": [
    {"T1": 50000.0, "T2": 30000.0},
    {"T1": 45000.0, "T2": 28000.0}
  ],
  "gates": {
    "h":      {"duration_ns": 35.0,  "error_rate": 0.001},
    "cx_0_1": {"duration_ns": 300.0, "error_rate": 0.01}
  },
  "coupling_map": [[0, 1]]
}"""


class TestInteractiveRegression(unittest.TestCase):
    """Regression: Interactive gate chain must still work after Phase 8 changes."""

    def test_interactive_bell_statevector(self):
        # : qr.interactive chain after initialize(). initialize() returns
        # None (void in C++); subsequent calls chain on the Interactive object.
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.interactive.initialize(2)
        qr.interactive.h(0).cx(0, 1)
        sv = qr.interactive.get_statevector()
        # Bell state |Phi+> = (|00>+|11>)/sqrt(2): index 0 and 3 have amp 1/sqrt(2).
        self.assertAlmostEqual(abs(sv[0]), 1.0 / math.sqrt(2), delta=1e-9)
        self.assertAlmostEqual(abs(sv[3]), 1.0 / math.sqrt(2), delta=1e-9)
        self.assertAlmostEqual(abs(sv[1]), 0.0, delta=1e-9)
        self.assertAlmostEqual(abs(sv[2]), 0.0, delta=1e-9)


class TestNoiseModel(unittest.TestCase):
    """NoiseModel construction, chaining, and integration with QRegister."""

    def test_add_depolarizing_chaining(self):
        # add_depolarizing returns the same NoiseModel object for chaining.
        nm = (qplayer.NoiseModel()
              .add_depolarizing("h", 1e-3)
              .add_depolarizing("cx", 5e-3))
        self.assertTrue(nm.has_channels_for_gate("h"))
        self.assertTrue(nm.has_channels_for_gate("cx"))
        self.assertFalse(nm.has_channels_for_gate("t"))

    def test_set_noise_model_and_run(self):
        # QRegister.set_noise_model() followed by run() must not raise.
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        nm = qplayer.NoiseModel().add_depolarizing("h", 1e-3)
        qr.set_noise_model(nm)
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1)
        result = qr.run(circ, 512)
        # With noise the distribution shifts, but total shots must be preserved.
        self.assertEqual(sum(result.counts().values()), 512)

    def test_has_t1_t2_initially_false(self):
        # A fresh NoiseModel without load_device() has no T1/T2 channels.
        nm = qplayer.NoiseModel()
        self.assertFalse(nm.has_t1_t2())

    def test_add_channel_types(self):
        # Verify all channel-add methods accept the API without raising.
        nm = qplayer.NoiseModel()
        nm.add_amplitude_damping("h", 0.005)
        nm.add_phase_damping("t", 0.002)
        nm.add_bit_flip("x", 0.001)
        nm.add_phase_flip("z", 0.001)
        nm.add_readout_error(0, 0.01, 0.02)
        self.assertTrue(nm.has_channels_for_gate("h"))
        self.assertTrue(nm.has_channels_for_gate("t"))


class TestDeviceModel(unittest.TestCase):
    """DeviceModel parsing and query API."""

    def setUp(self):
        self.dm = qplayer.DeviceModel.from_json_string(_DEVICE_JSON)

    def test_num_qubits(self):
        self.assertEqual(self.dm.num_qubits(), 2)

    def test_t1_t2_values(self):
        self.assertAlmostEqual(self.dm.T1(0), 50000.0, delta=1e-6)
        self.assertAlmostEqual(self.dm.T2(0), 30000.0, delta=1e-6)
        self.assertAlmostEqual(self.dm.T1(1), 45000.0, delta=1e-6)
        self.assertAlmostEqual(self.dm.T2(1), 28000.0, delta=1e-6)

    def test_unknown_qubit_returns_infinity(self):
        import math
        self.assertTrue(math.isinf(self.dm.T1(99)))
        self.assertTrue(math.isinf(self.dm.T2(99)))

    def test_coupling_map(self):
        self.assertTrue(self.dm.has_coupling_map())
        cm = self.dm.coupling_map()
        self.assertEqual(len(cm), 1)
        self.assertIn((0, 1), cm)

    def test_gate_property(self):
        gp = self.dm.gate_property("h", [])
        self.assertAlmostEqual(gp.duration_ns, 35.0, delta=1e-9)
        self.assertAlmostEqual(gp.error_rate,  0.001, delta=1e-12)

    def test_load_device_into_noise_model(self):
        # DeviceModel.load_device() synthesises T1/T2 channels into NoiseModel.
        nm = qplayer.NoiseModel()
        nm.load_device(self.dm)
        self.assertTrue(nm.has_t1_t2())


class TestConnectivityErrorRegistration(unittest.TestCase):
    """ConnectivityError must be importable and properly subclass CircuitError."""

    def test_connectivity_error_exists(self):
        # qplayer.ConnectivityError must be importable as a Python exception class.
        self.assertTrue(hasattr(qplayer, "ConnectivityError"))

    def test_connectivity_error_is_circuit_error_subclass(self):
        # Python-level hierarchy mirrors C++ hierarchy (error.hpp:54).
        # `except qplayer.CircuitError` must catch ConnectivityError.
        self.assertTrue(
            issubclass(qplayer.ConnectivityError, qplayer.CircuitError),
            "ConnectivityError must be a subclass of CircuitError")

    def test_connectivity_error_is_qplayer_error_subclass(self):
        self.assertTrue(
            issubclass(qplayer.ConnectivityError, qplayer.QPlayerError),
            "ConnectivityError must be a subclass of QPlayerError")

    def test_circuit_error_is_qplayer_error_subclass(self):
        self.assertTrue(
            issubclass(qplayer.CircuitError, qplayer.QPlayerError))


class TestGPUErrorRegistration(unittest.TestCase):
    """GPUError must be importable and subclass SimulatorError."""

    def test_gpu_error_exists(self):
        self.assertTrue(hasattr(qplayer, "GPUError"))

    def test_gpu_error_is_simulator_error_subclass(self):
        self.assertTrue(
            issubclass(qplayer.GPUError, qplayer.SimulatorError),
            "GPUError must be a subclass of SimulatorError")


class TestLoadDeviceModel(unittest.TestCase):
    """QRegister.load_device_model() integration test."""

    def test_load_valid_json_file(self):
        # Write a temp device file and load it; must not raise.
        with tempfile.NamedTemporaryFile(
                mode="w", suffix=".json", delete=False) as f:
            f.write(_DEVICE_JSON)
            path = f.name
        try:
            qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
            qr.load_device_model(path)
            # After load, run() must work (noise model is activated).
            circ = qplayer.Circuit(2, 2)
            circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1)
            result = qr.run(circ, 128)
            self.assertEqual(sum(result.counts().values()), 128)
        finally:
            os.unlink(path)

    def test_load_nonexistent_file_raises_circuit_error(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        with self.assertRaises(qplayer.CircuitError):
            qr.load_device_model("/nonexistent/path/device.json")


class TestChannelTypeEnum(unittest.TestCase):
    """ChannelType enum must be importable and have expected values."""

    def test_channel_type_values_exist(self):
        for name in ("Identity", "Depolarizing", "AmplitudeDamping",
                     "PhaseDamping", "BitFlip", "PhaseFlip", "Readout", "Custom"):
            self.assertTrue(
                hasattr(qplayer.ChannelType, name),
                f"ChannelType.{name} missing")


if __name__ == "__main__":
    unittest.main(verbosity=2)
