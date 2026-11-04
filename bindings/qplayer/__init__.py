"""QPlayer Python package — re-exports the pybind11 native module.

Phase 8 (Caracal-v5.1.0): Batch API + FullState CPU/GPU + SparseState
+ Interactive + NoiseModel + DeviceModel + full exception hierarchy.
 early expose: GateType, Op variant types (GateOp, MeasureOp,
BarrierOp, IfOp, IdleOp), Circuit.ops(), and Interactive facade.

Phase 9: TensorNetwork backend usable (MPS execution),
Result.truncation_errors() / has_truncation_errors(), and
QRegister.amplitude() / reduced_density_matrix().

Phase 10 / 10-A: TN noise (MPO and Monte Carlo trajectory) plus mid-circuit
measurement collapse, Reset and classical conditionals (,
).

Phase 11 (Lynx-v6.3.0): binding surface complete — opaque declare /
define / call and the read-only OpaqueDefinition view, the full six-class
exception hierarchy (QASMParseError, CircuitError, ConnectivityError,
SimulatorError, GPUError, NotImplementedError), zero-copy raw statevector
views, and py.typed.
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
    NoiseDrawEvent,
    NoiseModel,
    Observable,
    OpaqueDefinition,
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
    "NoiseDrawEvent",
    "NoiseModel",
    "Observable",
    "OpaqueDefinition",
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
