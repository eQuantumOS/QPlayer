"""QPlayerBackend — a Qiskit BackendV2 backed by QPlayer's simulator.

Declaring a BackendV2 makes QPlayer a drop-in execution target: users keep their
pure-Qiskit circuits and only swap the backend object. ``transpile(qc,
backend=...)`` decomposes into QPlayer's gate set (the Target below), and Qiskit's
BackendSamplerV2 / BackendEstimatorV2 wrap this backend for free (no dedicated
primitive classes). BackendV2 + JobV1 are the surface used here; both exist
unchanged on qiskit 1.1+ (1.x and 2.x), so the same code runs on either
(qiskit >= 1.1).
"""
from __future__ import annotations

import uuid

import qplayer as _qp
from qiskit.circuit import Measure, Parameter, Reset
from qiskit.circuit.library import standard_gates as _sg
from qiskit.providers import BackendV2, Options
from qiskit.transpiler import Target

from .job import QPlayerJob

# Default Target width. Gates are added as ideal global instructions
# (properties=None => all-to-all, no per-qubit table), so a wide Target is cheap.
_DEFAULT_NUM_QUBITS = 32


def _build_target(num_qubits):
    """QPlayer's native gate set as an all-to-all ideal Target (gate set)."""
    target = Target(num_qubits=num_qubits)
    th, ph, lm = Parameter("th"), Parameter("ph"), Parameter("lm")
    gates = [
        _sg.IGate(), _sg.HGate(), _sg.XGate(), _sg.YGate(), _sg.ZGate(),
        _sg.SGate(), _sg.SdgGate(), _sg.TGate(), _sg.TdgGate(),
        _sg.SXGate(), _sg.SXdgGate(),
        _sg.RXGate(th), _sg.RYGate(th), _sg.RZGate(th),
        _sg.PhaseGate(th), _sg.UGate(th, ph, lm),
        _sg.CXGate(), _sg.CYGate(), _sg.CZGate(), _sg.CHGate(), _sg.SwapGate(),
        _sg.CRZGate(th), _sg.CPhaseGate(th),
        _sg.CCXGate(), _sg.CSwapGate(),
    ]
    for gate in gates:
        target.add_instruction(gate, None)  # None => ideal, available everywhere
    target.add_instruction(Measure(), None)
    target.add_instruction(Reset(), None)
    return target


def _resolve_sim_type(engine):
    if engine is None or isinstance(engine, _qp.SimType):
        return engine or _qp.SimType.FullState
    key = str(engine).lower()
    return {"fullstate": _qp.SimType.FullState,
            "sparsestate": _qp.SimType.SparseState,
            "sparse": _qp.SimType.SparseState,
            "tensornetwork": _qp.SimType.TensorNetwork,
            "tn": _qp.SimType.TensorNetwork}.get(key, _qp.SimType.FullState)


def _resolve_proc(proc):
    if proc is None or isinstance(proc, _qp.ProcType):
        return proc or _qp.ProcType.CPU
    key = str(proc).lower()
    return {"cpu": _qp.ProcType.CPU, "gpu": _qp.ProcType.GPU,
            "auto": _qp.ProcType.Auto}.get(key, _qp.ProcType.CPU)


class QPlayerBackend(BackendV2):
    """BackendV2 mapping Qiskit execution onto ``QRegister(sim_type, proc).run``."""

    def __init__(self, provider=None, num_qubits=_DEFAULT_NUM_QUBITS,
                 engine=None, proc=None):
        super().__init__(provider=provider, name="qplayer", backend_version="0",
                         description="QPlayer full-state simulator backend")
        self._num_qubits = num_qubits
        self._target = _build_target(num_qubits)
        self._sim_type = _resolve_sim_type(engine)
        self._proc = _resolve_proc(proc)

    @property
    def target(self):
        return self._target

    @property
    def max_circuits(self):
        return 1  # single-circuit start; batch is a follow-up

    @classmethod
    def _default_options(cls):
        return Options(shots=1024, seed_simulator=None, memory=True)

    def run(self, run_input, **options):
        circuits = run_input if isinstance(run_input, (list, tuple)) else [run_input]
        if len(circuits) > 1:
            # max_circuits=1: honor the first, surface the truncation explicitly.
            import warnings
            warnings.warn("QPlayerBackend runs one circuit per job (max_circuits=1); "
                          "extra circuits are ignored.", stacklevel=2)
        shots = options.get("shots", self.options.shots)
        seed = options.get("seed_simulator", self.options.seed_simulator)
        sim_type = _resolve_sim_type(options.get("engine", self._sim_type))
        proc = _resolve_proc(options.get("proc", self._proc))
        return QPlayerJob(self, str(uuid.uuid4()), circuits[0], shots, seed,
                          sim_type, proc)
