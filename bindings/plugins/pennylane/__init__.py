"""QPlayer as a PennyLane device (Phase 7-K).

Usage (registered via the ``pennylane.plugins`` entry point)::

    import pennylane as qml
    dev = qml.device("qplayer.qubit", wires=2)        # or proc="gpu"
    @qml.qnode(dev)
    def circuit(x):
        qml.RY(x, 0); qml.CNOT([0, 1])
        return qml.expval(qml.Z(0) @ qml.Z(1))

Optional runtime dependency (``pip install qplayer[pennylane]``, pennylane >= 0.35).
The core ``import qplayer`` never imports pennylane — this subpackage is the only
place pennylane is loaded, and only when explicitly imported.
"""
from __future__ import annotations

try:
    import pennylane as _pennylane  # noqa: F401  (presence probe + clear error)
except ImportError as exc:  # pragma: no cover - exercised only without pennylane
    raise ImportError(
        "qplayer.pennylane requires PennyLane (>= 0.35). Install it with "
        "'pip install qplayer[pennylane]' or 'pip install pennylane>=0.35'."
    ) from exc

from .device import QPlayerDevice

__all__ = ["QPlayerDevice"]
