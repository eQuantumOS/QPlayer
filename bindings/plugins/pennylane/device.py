"""QPlayerDevice — a PennyLane device backed by QPlayer.

Implements the modern ``pennylane.devices.Device`` API. ``preprocess`` decomposes
the tape into QPlayer's gate set; ``execute`` evolves the state once and serves
every measurement: state/probs analytically (via the measurement's own
process_state), expval/var exactly (weighted Pauli sum on the evolved state,
qplayer Observable semantics), sample/counts from a finite-shot run.

Bit-ordering note: QPlayer's statevector index puts qubit 0 in the least-
significant position; PennyLane's process_state/process_samples take a wire_order
whose first entry is most-significant. Passing the device wires in reverse makes
QPlayer's native ordering line up with no manual bit reversal.
"""
from __future__ import annotations

import numpy as np
import qplayer as _qp
import pennylane as qml
from pennylane.devices import Device, DefaultExecutionConfig
from pennylane.devices.preprocess import (decompose, validate_device_wires,
                                          validate_measurements)
from pennylane.measurements import (CountsMP, ExpectationMP, ProbabilityMP,
                                     SampleMP, StateMP, VarianceMP)
from pennylane.tape import QuantumScript
from pennylane.transforms.core import TransformProgram

from .converter import (obs_to_pauli_terms, supported, to_qplayer_circuit)

_DEFAULT_SHOTS_FOR_SAMPLE = 1024


def _resolve_sim_type(engine):
    if engine is None or isinstance(engine, _qp.SimType):
        return engine or _qp.SimType.FullState
    key = str(engine).lower()
    return {"fullstate": _qp.SimType.FullState,
            "sparsestate": _qp.SimType.SparseState, "sparse": _qp.SimType.SparseState,
            "tensornetwork": _qp.SimType.TensorNetwork, "tn": _qp.SimType.TensorNetwork
            }.get(key, _qp.SimType.FullState)


def _resolve_proc(proc):
    if proc is None or isinstance(proc, _qp.ProcType):
        return proc or _qp.ProcType.CPU
    return {"cpu": _qp.ProcType.CPU, "gpu": _qp.ProcType.GPU,
            "auto": _qp.ProcType.Auto}.get(str(proc).lower(), _qp.ProcType.CPU)


class QPlayerDevice(Device):
    """PennyLane device executing tapes on ``QRegister(engine, proc)``."""

    name = "qplayer.qubit"

    def __init__(self, wires=None, shots=None, engine=None, proc=None, seed=None):
        super().__init__(wires=wires, shots=shots)
        self._sim_type = _resolve_sim_type(engine)
        self._proc = _resolve_proc(proc)
        self._seed = seed

    def preprocess(self, execution_config=DefaultExecutionConfig):
        program = TransformProgram()
        program.add_transform(validate_device_wires, self.wires, name=self.name)
        program.add_transform(decompose, stopping_condition=supported, name=self.name)
        program.add_transform(validate_measurements, name=self.name)
        return program, execution_config

    def execute(self, circuits, execution_config=DefaultExecutionConfig):
        single = isinstance(circuits, QuantumScript)
        tapes = [circuits] if single else list(circuits)
        results = [self._execute_tape(t) for t in tapes]
        return results[0] if single else tuple(results)

    def _wire_map(self, tape):
        wires = self.wires if self.wires else tape.wires
        ordered = list(wires)
        return ordered, {w: i for i, w in enumerate(ordered)}

    def _execute_tape(self, tape):
        ordered, w2q = self._wire_map(tape)
        n = len(ordered)
        # process_state/process_samples want the most-significant wire first; that
        # matches QPlayer's native ordering when device wires are reversed.
        rev_order = qml.wires.Wires(list(reversed(ordered)))
        needs_sample = any(isinstance(m, (SampleMP, CountsMP)) for m in tape.measurements)

        # Analytic evolution (no measurements) -> statevector for state/probs/expval/var.
        qr = _qp.QRegister(self._sim_type, self._proc)
        if self._seed is not None:
            qr.set_seed(self._seed)
        qr.run(to_qplayer_circuit(tape, w2q, n, with_measure=False), 1)
        statevector = np.asarray(qr.get_statevector())

        samples = None
        if needs_sample:
            shots = tape.shots.total_shots or _DEFAULT_SHOTS_FOR_SAMPLE
            qr_s = _qp.QRegister(self._sim_type, self._proc)
            qr_s.set_seed(self._seed if self._seed is not None else 1234)
            counts = dict(qr_s.run(
                to_qplayer_circuit(tape, w2q, n, with_measure=True), shots).counts())
            rows = []
            for key, count in counts.items():
                # key[0] is the highest qubit -> aligns with rev_order.
                rows.extend([[int(ch) for ch in key]] * count)
            samples = np.array(rows, dtype=np.int64)

        results = [self._measure(mp, qr, statevector, samples, rev_order, w2q, n)
                   for mp in tape.measurements]
        return results[0] if len(results) == 1 else tuple(results)

    def _measure(self, mp, qr, statevector, samples, rev_order, w2q, n):
        if isinstance(mp, ExpectationMP):
            return self._expval(qr, mp.obs, w2q, n)
        if isinstance(mp, VarianceMP):
            ev = self._expval(qr, mp.obs, w2q, n)
            ev_sq = self._expval(qr, (mp.obs @ mp.obs).simplify(), w2q, n)
            return ev_sq - ev * ev
        if isinstance(mp, (StateMP, ProbabilityMP)):
            return mp.process_state(statevector, wire_order=rev_order)
        if isinstance(mp, (SampleMP, CountsMP)):
            return mp.process_samples(samples, wire_order=rev_order)
        raise NotImplementedError(f"measurement {type(mp).__name__} not supported")

    def _expval(self, qr, obs, w2q, n):
        # Weighted Pauli sum on the evolved state == qplayer Observable's Sum(c_i <P_i>)
        # exact path (; QRegister.expectation_value(pauli) reads the state).
        total = 0.0
        for pauli, coeff in obs_to_pauli_terms(obs, w2q, n):
            ev = 1.0 if set(pauli) == {"I"} else qr.expectation_value(pauli)
            total += coeff * ev
        return total
