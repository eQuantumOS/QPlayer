"""QPlayer Python package — re-exports the pybind11 native module.

Lynx-v6.0.0 binding surface:

- Batch API over FullState CPU/GPU, SparseState and TensorNetwork (MPS),
  plus the Interactive facade, NoiseModel and DeviceModel.
- GateType, the Op variant types (GateOp, MeasureOp, BarrierOp, IfOp,
  IdleOp) and Circuit.ops().
- TensorNetwork queries: Result.truncation_errors() / has_truncation_errors()
  and QRegister.amplitude() / reduced_density_matrix().
- TensorNetwork noise (MPO and Monte Carlo trajectory), mid-circuit
  measurement collapse, Reset and classical conditionals.
- Opaque declare / define / call and the read-only OpaqueDefinition view.
- The full six-class exception hierarchy (QASMParseError, CircuitError,
  ConnectivityError, SimulatorError, GPUError, NotImplementedError),
  zero-copy raw statevector views, and py.typed.
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
