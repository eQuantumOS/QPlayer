"""QPlayer Python package — re-exports the pybind11 native module.

Phase 8 (Caracal-v5.0.0): Batch API + FullState CPU/GPU + SparseState
+ Interactive + NoiseModel + DeviceModel + full exception hierarchy.
 early expose: GateType, Op variant types (GateOp, MeasureOp,
BarrierOp, IfOp, IdleOp), Circuit.ops(), and Interactive facade.

Deferred to later phases:
  Phase 9  — TensorNetwork backend, Result.truncation_errors()
  Phase 10 — TN noise (MPO/Trajectory)
  Phase 11 — opaque definitions, py.typed, examples, sdist formal verification
"""

from ._qplayer import (
    BarrierOp,
    ChannelType,
    Circuit,
    ConnectivityError,
    DeviceModel,
    Estimator,
    GateOp,
    GateProperty,
    GateType,
    GPUError,
    IdleOp,
    IfOp,
    Interactive,
    KrausChannel,
    MeasureOp,
    NoiseModel,
    Observable,
    ProcType,
    QPlayerError,
    QASMParseError,
    CircuitError,
    SimulatorError,
    NotImplementedError,
    QRegister,
    Result,
    Sampler,
    SimType,
    __version__,
)

__all__ = [
    "BarrierOp",
    "ChannelType",
    "Circuit",
    "ConnectivityError",
    "DeviceModel",
    "Estimator",
    "GateOp",
    "GateProperty",
    "GateType",
    "GPUError",
    "IdleOp",
    "IfOp",
    "Interactive",
    "KrausChannel",
    "MeasureOp",
    "NoiseModel",
    "Observable",
    "ProcType",
    "QPlayerError",
    "QASMParseError",
    "CircuitError",
    "SimulatorError",
    "NotImplementedError",
    "QRegister",
    "Result",
    "Sampler",
    "SimType",
    "__version__",
]
