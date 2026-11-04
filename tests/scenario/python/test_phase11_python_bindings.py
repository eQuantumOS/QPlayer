"""Phase 11 Python binding tests (+ Phase 11 row).

 adds the state-access and threading half at the bottom of this file:
numpy dtype consistency, the zero-copy ``get_statevector_raw()`` views
and GIL release on ``run()`` / ``transpile()``.

Covers the two halves of :

  * **opaque surface** — the six entry points maps 1:1 from C++:
    ``declare_opaque`` · ``define_opaque`` single dispatcher over the
    four C++ overloads · ``opaque`` call
    ``has_opaque_definition`` / ``opaque_definition`` (singular) /
    ``opaque_definitions``. Plus ``OpaqueDefinition.body`` as the read-only
    3-variant: ``None`` for the DeclarationOnly stub, complex128 ndarray for a
    matrix, ``KrausChannel`` otherwise.
  * **exception hierarchy** — all six types catchable by name with their
    subclass relationships intact (``ConnectivityError`` under ``CircuitError``,
    ``GPUError`` under ``SimulatorError``, everything under ``QPlayerError``).
    The registration already existed; these tests pin it against regression.

Uses the stdlib ``unittest`` framework (pytest is not a build/test dependency):

    python -m unittest tests.scenario.python.test_phase11_python_bindings -v
"""

import gc
import os
import sys
import threading
import time
import unittest

import numpy as np

try:
    import qplayer  # noqa: E402
except ModuleNotFoundError:
    _REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    _DEV_PKG = os.path.join(_REPO_ROOT, "bindings")
    if _DEV_PKG not in sys.path:
        sys.path.insert(0, _DEV_PKG)
    import qplayer  # noqa: E402


X_2X2 = np.array([[0, 1],
                  [1, 0]], dtype=np.complex128)

CX_4X4 = np.array([[1, 0, 0, 0],
                   [0, 1, 0, 0],
                   [0, 0, 0, 1],
                   [0, 0, 1, 0]], dtype=np.complex128)


def _bitflip_channel(target=0):
    """A trace-preserving 1Q Kraus channel: sqrt(1-p) I and sqrt(p) X."""
    p = 0.25
    ch = qplayer.KrausChannel()
    a, b = (1.0 - p) ** 0.5, p ** 0.5
    ch.operators = [[[a + 0j, 0j], [0j, a + 0j]],
                    [[0j, b + 0j], [b + 0j, 0j]]]
    ch.target_qubit = target
    ch.type = qplayer.ChannelType.BitFlip
    return ch


class OpaqueDeclareAndPromote(unittest.TestCase):
    """declare_opaque stub -> define_opaque promotion (single direction)."""

    def test_declare_opaque_registers_stub_with_none_body(self):
        # Arrange
        circ = qplayer.Circuit(2, 2)

        # Act
        circ.declare_opaque("g", 1)
        definition = circ.opaque_definition("g")

        # Assert — the stub body is None, not an empty marker object.
        self.assertIsNone(definition.body)
        self.assertEqual(definition.name, "g")
        self.assertEqual(definition.num_qubits, 1)

    def test_has_opaque_definition_answers_registration_not_executability(self):
        # A stub answers True: the name is registered even though it has no body.
        # This pins the semantics corrected the C++ docstring to match.
        circ = qplayer.Circuit(2, 2)
        circ.declare_opaque("g", 1)

        self.assertTrue(circ.has_opaque_definition("g"))
        self.assertIsNone(circ.opaque_definition("g").body)

    def test_define_opaque_promotes_stub_in_place(self):
        # Arrange
        circ = qplayer.Circuit(2, 2)
        circ.declare_opaque("g", 1)

        # Act
        circ.define_opaque("g", X_2X2)

        # Assert — promoted in place, so still exactly one definition.
        self.assertEqual(len(circ.opaque_definitions()), 1)
        self.assertIsNotNone(circ.opaque_definition("g").body)

    def test_second_define_opaque_is_rejected(self):
        # : promotion is single-direction and happens exactly once.
        circ = qplayer.Circuit(2, 2)
        circ.define_opaque("g", X_2X2)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("g", X_2X2)

    def test_opaque_definition_rejects_unknown_name(self):
        circ = qplayer.Circuit(2, 2)

        with self.assertRaises(qplayer.CircuitError):
            circ.opaque_definition("never_declared")


class OpaqueDefineDispatcher(unittest.TestCase):
    """define_opaque dispatches over the four C++ overloads by shape/type."""

    def test_one_qubit_matrix_body_is_complex128_ndarray(self):
        # Arrange
        circ = qplayer.Circuit(2, 2)

        # Act
        circ.define_opaque("g1", X_2X2)
        body = circ.opaque_definition("g1").body

        # Assert
        self.assertIsInstance(body, np.ndarray)
        self.assertEqual(body.dtype, np.complex128)
        self.assertEqual(body.shape, (2, 2))
        np.testing.assert_allclose(body, X_2X2, atol=1e-15)

    def test_two_qubit_matrix_round_trips(self):
        circ = qplayer.Circuit(2, 2)

        circ.define_opaque("g2", CX_4X4)
        definition = circ.opaque_definition("g2")

        self.assertEqual(definition.num_qubits, 2)
        np.testing.assert_allclose(definition.body, CX_4X4, atol=1e-15)

    def test_n_qubit_matrix_requires_explicit_num_qubits(self):
        # : above 2 qubits the width must be stated, so a large
        # accidental array cannot register silently.
        circ = qplayer.Circuit(3, 3)
        identity8 = np.eye(8, dtype=np.complex128)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("g3", identity8)

        circ.define_opaque("g3", identity8, 3)
        self.assertEqual(circ.opaque_definition("g3").num_qubits, 3)

    def test_num_qubits_disagreeing_with_shape_is_rejected(self):
        circ = qplayer.Circuit(3, 3)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("g", np.eye(8, dtype=np.complex128), 2)

    def test_kraus_channel_body_is_the_channel(self):
        # Arrange
        circ = qplayer.Circuit(1, 1)

        # Act
        circ.define_opaque("noisy", _bitflip_channel())
        body = circ.opaque_definition("noisy").body

        # Assert
        self.assertIsInstance(body, qplayer.KrausChannel)
        self.assertEqual(len(body.operators), 2)

    def test_kraus_channel_rejects_num_qubits_argument(self):
        # The channel carries its own width; accepting a second source would let
        # the two disagree.
        circ = qplayer.Circuit(1, 1)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("noisy", _bitflip_channel(), 1)

    def test_nested_list_is_accepted_like_an_ndarray(self):
        # forcecast lets a plain nested sequence through — Python callers should
        # not have to import numpy just to define a 2x2.
        circ = qplayer.Circuit(1, 1)

        circ.define_opaque("g", [[0, 1], [1, 0]])

        np.testing.assert_allclose(circ.opaque_definition("g").body, X_2X2, atol=1e-15)

    def test_non_square_matrix_is_rejected(self):
        circ = qplayer.Circuit(2, 2)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("g", np.zeros((2, 4), dtype=np.complex128))

    def test_non_power_of_two_matrix_is_rejected(self):
        circ = qplayer.Circuit(2, 2)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("g", np.eye(3, dtype=np.complex128), 2)

    def test_non_unitary_matrix_is_rejected(self):
        # Validation lives in C++ define_opaque; the binding must not bypass it.
        circ = qplayer.Circuit(1, 1)

        with self.assertRaises(qplayer.CircuitError):
            circ.define_opaque("g", np.array([[2, 0], [0, 1]], dtype=np.complex128))


class OpaqueCall(unittest.TestCase):
    """Circuit.opaque() places the call op."""

    def test_opaque_call_appends_op_and_chains(self):
        # Arrange
        circ = qplayer.Circuit(2, 2)
        circ.define_opaque("g", X_2X2)

        # Act — chaining must survive the vector wrapper.
        returned = circ.opaque("g", [0]).opaque("g", [1])

        # Assert
        self.assertEqual(circ.num_ops(), 2)
        self.assertIs(returned, circ)

    def test_opaque_call_without_definition_is_deferred_to_transpile(self):
        # keeps QASM v2's "declare first, define later" meaning, so
        # the call itself is legal and SemanticValidatorPass rejects it later.
        circ = qplayer.Circuit(1, 1)
        circ.opaque("undefined_gate", [0])
        self.assertEqual(circ.num_ops(), 1)

        qr = qplayer.QRegister()
        with self.assertRaises(qplayer.CircuitError):
            qr.transpile(circ)

    def test_opaque_call_arity_mismatch_is_immediate(self):
        # A definition already exists, so the wrong operand count is a settled
        # error with no reason to wait for transpile().
        circ = qplayer.Circuit(2, 2)
        circ.define_opaque("g", X_2X2)

        with self.assertRaises(qplayer.CircuitError):
            circ.opaque("g", [0, 1])

    def test_opaque_call_rejects_empty_repeated_and_out_of_range_qubits(self):
        circ = qplayer.Circuit(2, 2)
        circ.define_opaque("g2", CX_4X4)

        with self.assertRaises(qplayer.CircuitError):
            circ.opaque("g2", [])
        with self.assertRaises(qplayer.CircuitError):
            circ.opaque("g2", [1, 1])
        with self.assertRaises(qplayer.CircuitError):
            circ.opaque("g2", [0, 9])

    def test_opaque_call_executes_on_fullstate(self):
        # The whole point of exposing define + call together: a Python-native
        # opaque circuit must actually run.
        circ = qplayer.Circuit(1, 1)
        circ.define_opaque("myx", X_2X2)
        circ.opaque("myx", [0])
        circ.measure(0, 0)

        counts = qplayer.QRegister(qplayer.SimType.FullState,
                                   qplayer.ProcType.CPU).run(circ, 512).counts()

        # X on |0> is deterministic, so every shot reads 1.
        self.assertEqual(counts.get("1", 0), 512)


class OpaqueQueryAndSerialization(unittest.TestCase):

    def test_opaque_definitions_lists_all_in_registration_order(self):
        circ = qplayer.Circuit(3, 3)
        circ.define_opaque("a", X_2X2)
        circ.define_opaque("b", CX_4X4)
        circ.declare_opaque("c", 1)

        listed = [(d.name, d.num_qubits, d.body is None) for d in circ.opaque_definitions()]

        self.assertEqual(listed, [("a", 1, False), ("b", 2, False), ("c", 1, True)])

    def test_body_is_read_only(self):
        # A writable body would let Python bypass the single-direction promotion
        # invariant define_opaque() enforces.
        circ = qplayer.Circuit(1, 1)
        circ.define_opaque("g", X_2X2)

        with self.assertRaises(AttributeError):
            circ.opaque_definition("g").body = None

    def test_definitions_survive_json_round_trip(self):
        # : opaque_definitions are part of the serialized circuit.
        circ = qplayer.Circuit(2, 2)
        circ.define_opaque("g", CX_4X4)
        circ.declare_opaque("stub", 1)

        restored = qplayer.Circuit.from_json(circ.to_json())

        self.assertTrue(restored.has_opaque_definition("g"))
        np.testing.assert_allclose(restored.opaque_definition("g").body, CX_4X4, atol=1e-15)
        self.assertIsNone(restored.opaque_definition("stub").body)


class ExceptionHierarchy(unittest.TestCase):
    """All six exception types are catchable by name with subclassing intact."""

    ALL_SIX = ("QASMParseError", "CircuitError", "SimulatorError",
               "NotImplementedError", "ConnectivityError", "GPUError")

    def test_all_six_types_are_exported_under_qplayer_error(self):
        for name in self.ALL_SIX:
            with self.subTest(exception=name):
                exc = getattr(qplayer, name, None)
                self.assertIsNotNone(exc, f"qplayer.{name} is not exported")
                self.assertTrue(issubclass(exc, qplayer.QPlayerError))

    def test_connectivity_error_is_a_circuit_error(self):
        # coupling violations must be catchable as plain CircuitError.
        self.assertTrue(issubclass(qplayer.ConnectivityError, qplayer.CircuitError))

    def test_gpu_error_is_a_simulator_error(self):
        self.assertTrue(issubclass(qplayer.GPUError, qplayer.SimulatorError))

    def test_qplayer_exceptions_do_not_shadow_builtins(self):
        # qplayer.NotImplementedError is its own type, not the builtin — mixing
        # them up would make `except NotImplementedError` catch the wrong thing.
        self.assertIsNot(qplayer.NotImplementedError, NotImplementedError)

    def test_parse_error_is_raised_and_catchable_by_name(self):
        qr = qplayer.QRegister()

        with self.assertRaises(qplayer.QASMParseError):
            qr.parse("this is not valid qasm")

    def test_circuit_error_is_raised_and_catchable_as_parent(self):
        circ = qplayer.Circuit(1, 1)

        with self.assertRaises(qplayer.QPlayerError):
            circ.opaque("g", [7])       # qubit out of range

    def test_raised_error_carries_a_message(self):
        circ = qplayer.Circuit(1, 1)

        with self.assertRaises(qplayer.CircuitError) as ctx:
            circ.opaque("g", [])

        self.assertIn("at least one qubit", str(ctx.exception))


# ── : numpy consistency · raw views · GIL ──────────────────────────────


def _prepared_register(n=3):
    """A FullState CPU register with a run() behind it, so state access is live."""
    qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
    circ = qplayer.Circuit(n, n)
    circ.h(0)
    for q in range(n - 1):
        circ.cx(q, q + 1)
    qr.run(circ, 16)
    return qr


class StatevectorDtype(unittest.TestCase):
    """AoS accessors are complex128 everywhere they appear."""

    def test_qregister_get_statevector_is_complex128(self):
        qr = _prepared_register()

        sv = qr.get_statevector()

        self.assertEqual(sv.dtype, np.complex128)
        self.assertEqual(sv.shape, (8,))

    def test_result_statevector_is_complex128(self):
        qr = _prepared_register()

        self.assertEqual(qr.result().statevector().dtype, np.complex128)

    def test_interactive_get_statevector_is_complex128(self):
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.interactive.initialize(2)
        qr.interactive.h(0)

        self.assertEqual(qr.interactive.get_statevector().dtype, np.complex128)

    def test_aos_accessor_owns_its_buffer(self):
        # get_statevector() is a copy by contract, unlike the raw views below —
        # it must survive a later run() unchanged.
        qr = _prepared_register()
        sv = qr.get_statevector()
        self.assertTrue(sv.flags.owndata)

        before = sv.copy()
        qr.run(qplayer.Circuit(2, 2).x(0), 4)

        np.testing.assert_array_equal(sv, before)


class StatevectorRawView(unittest.TestCase):
    """get_statevector_raw() returns zero-copy float64 views."""

    def test_returns_tuple_of_two_float64_arrays(self):
        qr = _prepared_register()

        raw = qr.get_statevector_raw()

        self.assertIsInstance(raw, tuple)
        self.assertEqual(len(raw), 2)
        for part in raw:
            self.assertEqual(part.dtype, np.float64)
            self.assertEqual(part.shape, (8,))

    def test_arrays_are_views_not_copies(self):
        qr = _prepared_register()

        re, im = qr.get_statevector_raw()

        self.assertFalse(re.flags.owndata)
        self.assertFalse(im.flags.owndata)

    def test_view_base_is_the_owning_register(self):
        # The base keeps the register alive; without it a view could outlive the
        # buffer it points into as soon as the last Python reference dropped.
        qr = _prepared_register()

        re, _ = qr.get_statevector_raw()

        self.assertIs(re.base, qr)

    def test_view_survives_dropping_the_register_reference(self):
        qr = _prepared_register()
        re, im = qr.get_statevector_raw()
        expected = float(re[0])

        del qr
        gc.collect()

        self.assertAlmostEqual(float(re[0]), expected, places=15)

    def test_views_are_read_only(self):
        # The C++ side hands out `const double*`; a writable view would let
        # Python mutate backend state behind the backend's own API.
        qr = _prepared_register()
        re, im = qr.get_statevector_raw()

        with self.assertRaises(ValueError):
            re[0] = 1.0
        with self.assertRaises(ValueError):
            im[0] = 1.0

    def test_raw_matches_the_aos_accessor(self):
        qr = _prepared_register()

        sv = qr.get_statevector()
        re, im = qr.get_statevector_raw()

        np.testing.assert_allclose(re + 1j * im, sv, atol=1e-15)

    def test_copy_is_how_callers_keep_amplitudes_across_calls(self):
        # The documented escape hatch: the view is invalidated by the next
        # backend call, .copy() is not.
        qr = _prepared_register()
        re, _ = qr.get_statevector_raw()
        kept = re.copy()

        qr.run(qplayer.Circuit(3, 3).x(0), 4)

        self.assertTrue(kept.flags.owndata)
        self.assertAlmostEqual(float(kept[0]), 2 ** -0.5, places=12)

    def test_interactive_exposes_raw_views_too(self):
        # puts the raw view on Interactive as well as QRegister.
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.interactive.initialize(2)
        qr.interactive.h(0)

        re, im = qr.interactive.get_statevector_raw()

        self.assertFalse(re.flags.owndata)
        self.assertEqual(re.dtype, np.float64)
        self.assertAlmostEqual(float(re[0]), 2 ** -0.5, places=12)

    def test_sparse_backend_rejects_raw_view(self):
        # SparseState has no SoA representation to view (backend table).
        qr = qplayer.QRegister(qplayer.SimType.SparseState)
        qr.run(qplayer.Circuit(2, 2).h(0).measure_all(), 8)

        with self.assertRaises(qplayer.SimulatorError):
            qr.get_statevector_raw()


class GilRelease(unittest.TestCase):
    """run() / transpile() release the GIL so other threads keep running."""

    @staticmethod
    def _ticks_during(work):
        """Run `work` while a sibling thread counts; return (elapsed, ticks, idle_rate)."""
        state = {"ticks": 0, "stop": False}

        def spin():
            while not state["stop"]:
                state["ticks"] += 1
                time.sleep(0.001)

        thread = threading.Thread(target=spin)
        thread.start()
        try:
            time.sleep(0.2)
            idle_rate = state["ticks"] / 0.2     # calibrate this machine's tick rate
            before = state["ticks"]
            start = time.perf_counter()
            work()
            elapsed = time.perf_counter() - start
            ticks = state["ticks"] - before
        finally:
            state["stop"] = True
            thread.join()
        return elapsed, ticks, idle_rate

    def _assert_progressed(self, elapsed, ticks, idle_rate):
        # A GIL-holding call starves the sibling: it would tick ~0 times. The
        # 0.3 factor leaves room for scheduler noise and OpenMP oversubscription
        # without letting a fully blocked thread pass.
        expected = idle_rate * elapsed
        self.assertGreater(ticks, expected * 0.3,
                           f"sibling thread got {ticks} ticks, expected ~{expected:.0f}")

    def test_run_releases_the_gil(self):
        # 24 qubits: long enough that the sibling accumulates a countable number
        # of ticks, short enough to keep the suite fast.
        circ = qplayer.Circuit(24, 24)
        circ.h(0)
        for q in range(23):
            circ.cx(q, q + 1)
        circ.measure_all()
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)

        elapsed, ticks, idle_rate = self._ticks_during(lambda: qr.run(circ, 1024))

        self._assert_progressed(elapsed, ticks, idle_rate)

    def test_transpile_releases_the_gil(self):
        # A deep circuit so the optimizer has real work to do.
        circ = qplayer.Circuit(12, 12)
        for _ in range(400):
            for q in range(11):
                circ.cx(q, q + 1)
            for q in range(12):
                circ.rz(0.1, q)
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)

        elapsed, ticks, idle_rate = self._ticks_during(
            lambda: qr.transpile(circ, 3))

        self._assert_progressed(elapsed, ticks, idle_rate)

    def test_run_result_is_unaffected_by_gil_release(self):
        # Releasing the GIL must not change numerics — bell stays bell.
        circ = qplayer.Circuit(2, 2)
        circ.h(0).cx(0, 1).measure_all()
        qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qr.set_seed(1234)

        counts = qr.run(circ, 4096).counts()

        self.assertEqual(set(counts) - {"00", "11"}, set())
        self.assertEqual(sum(counts.values()), 4096)


if __name__ == "__main__":
    unittest.main()
