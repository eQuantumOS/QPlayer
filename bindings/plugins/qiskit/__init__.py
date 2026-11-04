"""QPlayer as a Qiskit BackendV2 execution backend (Phase 7-J).

Usage (pure Qiskit, only the backend object changes)::

    from qplayer.qiskit import QPlayerProvider
    backend = QPlayerProvider().get_backend()          # or proc="gpu"
    tqc = transpile(qc, backend=backend)
    counts = backend.run(tqc, shots=1024).result().get_counts()

Optional runtime dependency (``pip install qplayer[qiskit]``, qiskit >= 1.1 —
both 1.x and 2.x). The plugin uses only the BackendV2 + JobV1 + base-class-free
duck-typed provider surface, which is unchanged across 1.x and 2.x, so the same
code runs on either with no version branch. The core
``import qplayer`` never imports qiskit — this subpackage is the only place
qiskit is loaded, and only when explicitly imported (core purity).
"""
from __future__ import annotations

try:
    import qiskit as _qiskit  # noqa: F401  (presence probe + clear error)
except ImportError as exc:  # pragma: no cover - exercised only without qiskit
    raise ImportError(
        "qplayer.qiskit requires Qiskit (>= 1.1). Install it with "
        "'pip install qplayer[qiskit]' or 'pip install \"qiskit>=1.1\"'."
    ) from exc

from .backend import QPlayerBackend
from .converter import to_qplayer_circuit
from .job import QPlayerJob
from .provider import QPlayerProvider

__all__ = [
    "QPlayerProvider",
    "QPlayerBackend",
    "QPlayerJob",
    "to_qplayer_circuit",
]
