"""QPlayerJob — runs the converted circuit on QPlayer and assembles a Qiskit Result.

The Result exposes both aggregated counts (``get_counts``) and per-shot memory
(``get_memory``); BackendSamplerV2 consumes the latter. QPlayer reports only
aggregated counts, so per-shot memory is reconstructed by expanding the count
multiset — order within a shot list is not statistically meaningful here.
"""
from __future__ import annotations

import qplayer as _qp
import qiskit as _qiskit
from qiskit.providers import JobV1, JobStatus
from qiskit.result import Result
from qiskit.result.models import ExperimentResult, ExperimentResultData

from .converter import to_qplayer_circuit


def _experiment_header(creg_sizes, memory_slots):
    """Header that makes ``Result.get_counts()`` pad bit width + group cregs on
    both qiskit 1.x and 2.x.

    The two majors read the header differently: 2.x accepts a plain dict, but 1.x
    drops the width of all-zero outcomes ('00' -> '0') unless the header is an
    attribute-style ``QobjExperimentHeader``. That class is deprecated in 1.x and
    removed in 2.0, so it is used only on the 1.x branch (warning suppressed since
    this is the sanctioned 1.x path); 2.x keeps the plain dict. Letting qiskit's
    own formatter split the registers keeps multi-creg spacing correct on either.
    """
    fields = {"creg_sizes": creg_sizes, "memory_slots": memory_slots}
    if int(_qiskit.__version__.split(".", 1)[0]) >= 2:
        return fields
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        from qiskit.qobj import QobjExperimentHeader
        return QobjExperimentHeader(**fields)


class QPlayerJob(JobV1):
    """Eager single-circuit job (max_circuits=1)."""

    def __init__(self, backend, job_id, circuit, shots, seed, sim_type, proc):
        super().__init__(backend, job_id)
        self._circ = circuit
        self._shots = shots
        self._seed = seed
        self._sim_type = sim_type
        self._proc = proc
        self._result = None

    def submit(self):
        # Execution is eager (in result()); nothing async to submit.
        pass

    def status(self):
        return JobStatus.DONE

    def result(self):
        if self._result is None:
            self._result = self._execute()
        return self._result

    def _execute(self):
        qc = self._circ
        qr = _qp.QRegister(self._sim_type, self._proc)
        if self._seed is not None:
            qr.set_seed(self._seed)  # reproducible sampling
        counts = dict(qr.run(to_qplayer_circuit(qc), self._shots).counts())

        # Expand counts -> per-shot hex memory. QPlayer count keys already use
        # Qiskit's bit order (clbit_last ... clbit_0), so int(key, 2) is the
        # memory value directly (— no bit reversal needed).
        counts_hex, memory = {}, []
        for bitstr, n in counts.items():
            clean = bitstr.replace(" ", "")
            hx = hex(int(clean, 2) if clean else 0)
            counts_hex[hx] = counts_hex.get(hx, 0) + n
            memory.extend([hx] * n)

        creg_sizes = [[cr.name, cr.size] for cr in qc.cregs] or [["c", qc.num_clbits]]
        header = _experiment_header(creg_sizes, qc.num_clbits)
        data = ExperimentResultData(counts=counts_hex, memory=memory)
        exp = ExperimentResult(shots=self._shots, success=True, data=data,
                               header=header, meas_level=2)
        return Result(backend_name=self._backend.name, backend_version="0",
                      job_id=self._job_id, success=True, results=[exp])
