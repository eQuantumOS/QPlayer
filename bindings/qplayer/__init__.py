"""QPlayer Python package — re-exports the pybind11 native module.

Caracal-v5.0.0: Batch API + FullState CPU/GPU + SparseState + Interactive +
NoiseModel + DeviceModel + full exception hierarchy. Also exposes GateType,
the Op variant types (GateOp, MeasureOp, BarrierOp, IfOp, IdleOp),
Circuit.ops(), and the Interactive facade.

The TensorNetwork backend and opaque definitions are not available in this
release; they arrive in Lynx-v6.0.0.
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
