// qplayer_bindings.cpp — Python bindings ( / / Interactive
// early expose · nonzero-amplitude enumeration).
//
// Phase 7 scope: expose Batch API + 3 backends
// (FullState CPU, FullState GPU, SparseState).
// early expose (Phase 7+): GateType, Op variants (GateOp /
// MeasureOp / BarrierOp / IfOp / IdleOp), Circuit.ops(), and the
// Interactive facade (qr.interactive) are added here as a forward-expose
// of the Interactive-mode feature.
// : get_statevector_nonzero() exposed on QRegister and Interactive as
// list[tuple[int, complex]]. StateIndex (__uint128_t) is converted losslessly
// to a Python arbitrary-precision int via hi/lo 64-bit decomposition.
// Phase 8: NoiseModel, DeviceModel, ConnectivityError, GPUError are
// now exposed. TensorNetwork, opaque definitions, and if_then remain deferred
// to Phase 9/11 as documented in.
//
// Exception mapping: Phase 8 adds ConnectivityError
// (CircuitError subtype) and GPUError (SimulatorError subtype,
//). Full 6-type hierarchy is now registered (arch).
//
// numpy integration: StateVector (vector<complex<double>>) is
// converted to numpy.ndarray(dtype=complex128) via py::array_t copy.
// RawStateView is returned as a (re: float64 ndarray, im: float64 ndarray)
// tuple copy. Zero-copy GIL-released view is deferred to Phase 11
// (Phase 11 row).

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <pybind11/complex.h>

#include "qregister.hpp"           // includes interactive.hpp via friend
#include "circuit/gate_op.hpp"
#include "observable.hpp"          //
#include "primitives.hpp"          // Sampler / Estimator facades
#include "error.hpp"
#include "noise/noise_model.hpp"   // NoiseModel / KrausChannel / ChannelType
#include "noise/device_model.hpp"  // DeviceModel / GateProperty / CouplingMap

namespace py = pybind11;

namespace {

// Register all 6 qplayer exception types as Python exceptions.
// SSOT: arch/, top notice box, Phase 8 row.
// Subclass relationships are modelled by passing the PyObject* of the parent
// exception as the third argument to py::register_exception (pybind11).
// ConnectivityError -> CircuitError -> QPlayerError
// GPUError          -> SimulatorError -> QPlayerError
void register_exceptions(py::module_& m) {
    // Root must be registered first so child types can reference its ptr().
    static py::exception<qplayer::QPlayerError> root(m, "QPlayerError");
    // Leaf types: pass root.ptr() to make them Python-subclasses of QPlayerError.
    auto circuit_exc = py::register_exception<qplayer::CircuitError>(
        m, "CircuitError", root.ptr());
    auto sim_exc = py::register_exception<qplayer::SimulatorError>(
        m, "SimulatorError", root.ptr());
    py::register_exception<qplayer::QASMParseError>    (m, "QASMParseError",     root.ptr());
    py::register_exception<qplayer::NotImplementedError>(m, "NotImplementedError", root.ptr());
    // ConnectivityError is a CircuitError subtype (error.hpp:54).
    // circuit_exc.ptr() as base: `except qplayer.CircuitError` catches it too.
    py::register_exception<qplayer::ConnectivityError>(m, "ConnectivityError", circuit_exc.ptr());
    // GPUError is a SimulatorError subtype.
    py::register_exception<qplayer::GPUError>          (m, "GPUError",           sim_exc.ptr());
}

// Convert StateVector (AoS complex<double>) to numpy complex128 ndarray.
// Returns a copy — zero-copy view with GIL release is Phase 11
// (Phase 11 row).
py::array_t<std::complex<double>> sv_to_numpy(const qplayer::StateVector& sv) {
    return py::array_t<std::complex<double>>(
        static_cast<py::ssize_t>(sv.size()), sv.data());
}

// Convert RawStateView (SoA re/im double*) to a tuple of two float64
// ndarrays. Returns copies; the raw pointer lifetime ends at this call.
// Phase 11 will add a zero-copy buffer protocol path.
py::tuple raw_to_numpy_tuple(const qplayer::RawStateView& rv) {
    auto n = static_cast<py::ssize_t>(rv.size);
    py::array_t<double> re_arr(n, rv.re);
    py::array_t<double> im_arr(n, rv.im);
    return py::make_tuple(re_arr, im_arr);
}

// Convert a 128-bit StateIndex to an arbitrary-precision Python int (lossless).
// pybind11 has no built-in __uint128_t converter, so we split into hi/lo 64-bit
// words and reconstruct via Python integer arithmetic.
// For the common case (hi == 0, i.e. fewer than 65 qubits) this is simply
// py::int_(lo); the full path handles circuits up to 128 qubits.
static py::object state_index_to_py(qplayer::StateIndex v) {
    const std::uint64_t lo = static_cast<std::uint64_t>(v);
    const std::uint64_t hi = static_cast<std::uint64_t>(v >> 64);
    if (hi == 0) {
        return py::int_(lo);
    }
    // Build (hi << 64) | lo using the Python C API so we stay in arbitrary-
    // precision integer arithmetic. PyNumber_* return a new reference (or NULL
    // on error); py::reinterpret_steal takes ownership and converts to py::object.
    py::object py_hi  = py::int_(hi);
    py::object py_64  = py::int_(static_cast<std::uint64_t>(64));
    PyObject*  raw_sh = PyNumber_Lshift(py_hi.ptr(), py_64.ptr());
    if (!raw_sh) throw py::error_already_set();
    py::object shifted = py::reinterpret_steal<py::object>(raw_sh);
    py::object py_lo   = py::int_(lo);
    PyObject*  raw_or  = PyNumber_Or(shifted.ptr(), py_lo.ptr());
    if (!raw_or) throw py::error_already_set();
    return py::reinterpret_steal<py::object>(raw_or);
}

// Convert a nonzero-amplitudes vector to a Python list[tuple[int, complex]].
// Each entry is (basis_index: int, amplitude: complex). The basis index is an
// arbitrary-precision Python int so 128-qubit StateIndex values are lossless.
static py::list nonzero_amplitudes_to_py(
        const std::vector<std::pair<qplayer::StateIndex,
                                    std::complex<double>>>& entries) {
    py::list result;
    for (const auto& [idx, amp] : entries) {
        result.append(py::make_tuple(state_index_to_py(idx),
                                     std::complex<double>{amp.real(), amp.imag()}));
    }
    return result;
}

}  // namespace

PYBIND11_MODULE(_qplayer, m) {
    m.doc() = "QPlayer Python bindings — Phase 8: Batch API + "
              "FullState CPU/GPU + SparseState + Interactive + "
              "NoiseModel + DeviceModel + full exception hierarchy. "
              "TensorNetwork, opaque, and if_then remain deferred "
              "(Phase 9/11).";

    register_exceptions(m);

    // ── SimType / ProcType enums ───────────────────────
    // SimType: 3 values. TensorNetwork is exposed for future
    // Phase 9 use; QRegister(SimType.TensorNetwork) raises SimulatorError
    // until Phase 9 activates it (Phase 9 row).
    py::enum_<qplayer::SimType>(m, "SimType")
        .value("FullState",     qplayer::SimType::FullState)
        .value("SparseState",   qplayer::SimType::SparseState)
        .value("TensorNetwork", qplayer::SimType::TensorNetwork);

    py::enum_<qplayer::ProcType>(m, "ProcType")
        .value("CPU",  qplayer::ProcType::CPU)
        .value("GPU",  qplayer::ProcType::GPU)
        .value("Auto", qplayer::ProcType::Auto);

    // ── GateType enum (gate_op.hpp early expose) ────────────────
    // Full set matching OpenQASM v2 qelib1.inc plus Reset/Opaque.
    // Exposed so Python code can inspect op.type on GateOp objects returned
    // by Circuit.ops() and dispatch via isinstance / op.type == GateType.H.
    py::enum_<qplayer::GateType>(m, "GateType")
        // 1-qubit Clifford+T
        .value("I",    qplayer::GateType::I)
        .value("X",    qplayer::GateType::X)
        .value("Y",    qplayer::GateType::Y)
        .value("Z",    qplayer::GateType::Z)
        .value("H",    qplayer::GateType::H)
        .value("S",    qplayer::GateType::S)
        .value("Sdg",  qplayer::GateType::Sdg)
        .value("T",    qplayer::GateType::T)
        .value("Tdg",  qplayer::GateType::Tdg)
        .value("SX",   qplayer::GateType::SX)
        .value("SXdg", qplayer::GateType::SXdg)
        // 1-qubit rotations / U gates
        .value("RX",   qplayer::GateType::RX)
        .value("RY",   qplayer::GateType::RY)
        .value("RZ",   qplayer::GateType::RZ)
        .value("U1",   qplayer::GateType::U1)
        .value("U2",   qplayer::GateType::U2)
        .value("U3",   qplayer::GateType::U3)
        // 2-qubit gates
        .value("CX",   qplayer::GateType::CX)
        .value("CY",   qplayer::GateType::CY)
        .value("CZ",   qplayer::GateType::CZ)
        .value("CH",   qplayer::GateType::CH)
        .value("SWAP", qplayer::GateType::SWAP)
        .value("CRZ",  qplayer::GateType::CRZ)
        .value("CU1",  qplayer::GateType::CU1)
        .value("CU3",  qplayer::GateType::CU3)
        // 3-qubit gates
        .value("CCX",   qplayer::GateType::CCX)
        .value("CSwap", qplayer::GateType::CSwap)
        // Non-unitary / user-defined
        .value("Reset",  qplayer::GateType::Reset)
        .value("Opaque", qplayer::GateType::Opaque);

    // ── Op variant alternatives — read-only, for Circuit.ops() iteration ──
    // (early expose). User code receives these objects from ops() and
    // forwards GateOp directly to qr.interactive.apply_gate(op). Construction
    // from Python is intentionally omitted; only the C++ IR is the source.

    // GateOp: type, name, qubits, params (noise_channels / duration_ns /
    // is_fused / is_opaque are implementation-internal and not exposed).
    py::class_<qplayer::GateOp>(m, "GateOp")
        .def_readonly("type",   &qplayer::GateOp::type)
        .def_readonly("name",   &qplayer::GateOp::name)
        .def_readonly("qubits", &qplayer::GateOp::qubits)
        .def_readonly("params", &qplayer::GateOp::params);

    py::class_<qplayer::MeasureOp>(m, "MeasureOp")
        .def_readonly("qubit", &qplayer::MeasureOp::qubit)
        .def_readonly("cubit", &qplayer::MeasureOp::cubit);

    py::class_<qplayer::BarrierOp>(m, "BarrierOp")
        .def_readonly("qubits", &qplayer::BarrierOp::qubits);

    // IfOp: creg index, value, and body GateOp.
    // body is returned by value so its lifetime is independent of the IfOp.
    py::class_<qplayer::IfOp>(m, "IfOp")
        .def_readonly("creg",  &qplayer::IfOp::creg)
        .def_readonly("value", &qplayer::IfOp::value)
        .def_readonly("body",  &qplayer::IfOp::body);

    py::class_<qplayer::IdleOp>(m, "IdleOp")
        .def_readonly("qubit",       &qplayer::IdleOp::qubit)
        .def_readonly("duration_ns", &qplayer::IdleOp::duration_ns);

    // ── Circuit (builder facade) ──────────────────────────
    // All gate methods return Circuit& for chaining (circ.h(0).cx(0,1)...).
    // py::return_value_policy::reference_internal ensures the returned
    // reference stays alive while the Circuit object is alive.
    py::class_<qplayer::Circuit>(m, "Circuit")
        .def(py::init<qplayer::QubitIndex, qplayer::ClassicalBitIndex>(),
             py::arg("num_qubits"), py::arg("num_cubits") = 0)

        // ── Single-qubit gates (qelib1.inc) ─────────────────
        .def("id",  &qplayer::Circuit::id,  py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("h",   &qplayer::Circuit::h,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("x",   &qplayer::Circuit::x,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("y",   &qplayer::Circuit::y,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("z",   &qplayer::Circuit::z,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("s",   &qplayer::Circuit::s,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("sdg", &qplayer::Circuit::sdg, py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("t",   &qplayer::Circuit::t,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("tdg", &qplayer::Circuit::tdg, py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("sx",   &qplayer::Circuit::sx,   py::arg("q"),      //
             py::return_value_policy::reference_internal)
        .def("sxdg", &qplayer::Circuit::sxdg, py::arg("q"),      //
             py::return_value_policy::reference_internal)

        // ── Single-qubit rotation / U gates (qelib1.inc) ─────────────
        .def("rx", &qplayer::Circuit::rx, py::arg("theta"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("ry", &qplayer::Circuit::ry, py::arg("theta"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("rz", &qplayer::Circuit::rz, py::arg("theta"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("u1", &qplayer::Circuit::u1, py::arg("lam"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("u2", &qplayer::Circuit::u2,
             py::arg("phi"), py::arg("lam"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("u3", &qplayer::Circuit::u3,
             py::arg("theta"), py::arg("phi"), py::arg("lam"), py::arg("q"),
             py::return_value_policy::reference_internal)

        // ── Two-qubit gates (qelib1.inc) ────────────────────
        .def("cx",   &qplayer::Circuit::cx,   py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cy",   &qplayer::Circuit::cy,   py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cz",   &qplayer::Circuit::cz,   py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("ch",   &qplayer::Circuit::ch,   py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("swap", &qplayer::Circuit::swap, py::arg("q0"),   py::arg("q1"),
             py::return_value_policy::reference_internal)
        .def("crz",  &qplayer::Circuit::crz,
             py::arg("theta"), py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cu1",  &qplayer::Circuit::cu1,
             py::arg("lam"), py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cu3",  &qplayer::Circuit::cu3,
             py::arg("theta"), py::arg("phi"), py::arg("lam"),
             py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)

        // ── Three-qubit gates (qelib1.inc) ──────────────────
        .def("ccx",   &qplayer::Circuit::ccx,
             py::arg("c0"), py::arg("c1"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cswap", &qplayer::Circuit::cswap,
             py::arg("ctrl"), py::arg("t0"), py::arg("t1"),
             py::return_value_policy::reference_internal)

        // ── Measurement / control flow ─────────────────────────────────
        .def("measure",     &qplayer::Circuit::measure,
             py::arg("q"), py::arg("cubit"),
             py::return_value_policy::reference_internal)
        .def("measure_all", &qplayer::Circuit::measure_all,
             py::return_value_policy::reference_internal)
        .def("reset",       &qplayer::Circuit::reset, py::arg("q"),
             py::return_value_policy::reference_internal)

        // Circuit::barrier() only accepts std::initializer_list, which cannot be
        // built from a runtime std::vector. Replicate its qubit-range validation
        // (CircuitError on out-of-range, mirroring check_qubit in circuit.cpp)
        // then push the BarrierOp via the detail bridge the parser uses — this is
        // correct for any arity. Empty qubits => "all qubits".
        .def("barrier",
             [](qplayer::Circuit& self,
                const std::vector<qplayer::QubitIndex>& qubits)
                     -> qplayer::Circuit& {
                 qplayer::BarrierOp b;
                 b.qubits.reserve(qubits.size());
                 const qplayer::QubitIndex nq = self.num_qubits();
                 for (qplayer::QubitIndex q : qubits) {
                     if (q >= nq) {
                         throw qplayer::CircuitError{
                             "barrier: qubit index out of range"};
                     }
                     b.qubits.push_back(q);
                 }
                 qplayer::detail_circuit_push_op(self,
                                                 qplayer::Op{std::move(b)});
                 return self;
             },
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal)

        // if_then: deferred (requires classical-register evaluation).
        // Opaque methods (declare_opaque, define_opaque, has_opaque_definition,
        // opaque_definition, opaque_definitions) are deferred to Phase 11
        // (Phase 11 row).

        // ── Classical register management ───────────────────
        .def("add_creg", &qplayer::Circuit::add_creg,
             py::arg("name"), py::arg("width"))
        .def("creg",     &qplayer::Circuit::creg,     py::arg("name"))
        .def("total_cubits",        &qplayer::Circuit::total_cubits)
        .def("num_qubits",          &qplayer::Circuit::num_qubits)
        .def("num_ops",             &qplayer::Circuit::num_ops)

        // ── Expectation value pre-registration ────
        .def("add_expectation",     &qplayer::Circuit::add_expectation,
             py::arg("pauli"),
             py::return_value_policy::reference_internal)
        .def("expectation_targets", &qplayer::Circuit::expectation_targets,
             py::return_value_policy::copy)

        // ── Op list access (early expose) ─────────────────────
        // ops() returns the raw IR as a Python list of concrete Op subtypes.
        // Each std::variant<GateOp,...> is converted to the matching Python
        // class (GateOp / MeasureOp / BarrierOp / IfOp / IdleOp) via
        // std::visit so isinstance() dispatch works on the Python side.
        // The list is a snapshot copy; mutating it does not affect the Circuit.
        .def("ops",
             [](const qplayer::Circuit& self) {
                 py::list result;
                 for (const qplayer::Op& op : self.ops()) {
                     std::visit([&result](const auto& v) {
                         result.append(py::cast(v));
                     }, op);
                 }
                 return result;
             })

        // ── Serialisation ───────────────
        .def("to_json",  &qplayer::Circuit::to_json)
        .def("to_qasm",  &qplayer::Circuit::to_qasm)
        .def_static("from_json",       &qplayer::Circuit::from_json,
                    py::arg("text"))
        .def_static("load_json_file",  &qplayer::Circuit::load_json_file,
                    py::arg("path"))

        // ── Diagram rendering ─────────────────────
        // Unified entry point (Qiskit circuit.draw(output=...) convention):
        //   output="text" -> str via the C++ Circuit::draw text renderer.
        //   output="mpl"  -> matplotlib Figure via the pure-Python renderer
        //                    qplayer.draw (optional dependency, qplayer[viz]).
        // The "mpl" branch defers to Python so the C++ core stays free of any
        // graphics dependency; it reads the circuit through to_json().
        .def("draw",
             [](const qplayer::Circuit& self, const std::string& output,
                const std::string& style, py::object filename) -> py::object {
                 const auto sty = (style == "ascii")
                     ? qplayer::CircuitDrawStyle::Ascii
                     : qplayer::CircuitDrawStyle::Unicode;
                 if (output == "text") {
                     return py::cast(self.draw(sty));
                 }
                 if (output == "mpl") {
                     py::module_ d = py::module_::import("qplayer.draw");
                     return d.attr("draw_mpl")(py::cast(self), style, filename);
                 }
                 throw std::invalid_argument(
                     "Circuit.draw: output must be 'text' or 'mpl', got '" + output + "'");
             },
             py::arg("output") = "text", py::arg("style") = "unicode",
             py::arg("filename") = py::none())
        .def("__str__",  [](const qplayer::Circuit& self) { return self.draw(); })
        .def("__repr__", [](const qplayer::Circuit& self) { return self.draw(); });

    // ── Result (read-only result container) ─────────
    // counts() and statevector() are essential for distribution checks.
    // truncation_errors() is deferred to Phase 9 (Phase 9 row).
    py::class_<qplayer::Result>(m, "Result")
        .def("shots",           &qplayer::Result::shots)
        .def("counts",          &qplayer::Result::counts,
             py::return_value_policy::copy)
        .def("has_statevector", &qplayer::Result::has_statevector)
        .def("statevector",
             [](const qplayer::Result& r) {
                 return sv_to_numpy(r.statevector());
             })
        // Expectation values.
        .def("has_expectation",   &qplayer::Result::has_expectation,
             py::arg("pauli"))
        .def("expectation",       &qplayer::Result::expectation,
             py::arg("pauli"))
        .def("all_expectations",  &qplayer::Result::all_expectations,
             py::return_value_policy::copy)
        // Error info (always trivial under kept for ABI).
        .def("has_error",         &qplayer::Result::has_error)
        .def("error_message",     &qplayer::Result::error_message)
        // has_truncation_errors is harmless to expose; actual content
        // (truncation_errors()) is deferred to Phase 9.
        .def("has_truncation_errors", &qplayer::Result::has_truncation_errors);

    // ── Interactive (early expose) ────────────────────
    // Interactive is non-copyable and owned exclusively by QRegister. It is
    // accessed via qr.interactive (reference_internal policy keeps the
    // QRegister alive as long as the Interactive reference is held).
    //
    // CPU statevector backends only (FullState CPU, SparseState). FullState
    // GPU and TensorNetwork throw SimulatorError from initialize(). 
    // Use the batch run() path for those backends.
    //
    // NoiseModel injection into Interactive is deferred to Phase 8.
    py::class_<qplayer::Interactive>(m, "Interactive")
        // ── State lifecycle ────────────────────────────────────────────
        .def("initialize", &qplayer::Interactive::initialize,
             py::arg("num_qubits"))
        .def("reset",
             py::overload_cast<>(&qplayer::Interactive::reset))
        .def("reset",
             py::overload_cast<qplayer::QubitIndex>(&qplayer::Interactive::reset),
             py::arg("q"))

        // ── Generic gate entry point ────────────────────────
        // apply_gate covers the full GateType enum including CH/SX/CRZ/CU1/
        // CU3/CSwap that lack typed helpers. Returns self for chaining.
        .def("apply_gate",
             &qplayer::Interactive::apply_gate,
             py::arg("op"),
             py::return_value_policy::reference_internal)

        // ── Single-qubit gates (chainable) ────────────────────────────
        .def("h",   &qplayer::Interactive::h,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("x",   &qplayer::Interactive::x,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("y",   &qplayer::Interactive::y,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("z",   &qplayer::Interactive::z,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("s",   &qplayer::Interactive::s,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("sdg", &qplayer::Interactive::sdg, py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("t",   &qplayer::Interactive::t,   py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("tdg", &qplayer::Interactive::tdg, py::arg("q"),
             py::return_value_policy::reference_internal)

        // ── Single-qubit rotation / U gates (chainable) ───────────────
        .def("rx",  &qplayer::Interactive::rx,
             py::arg("theta"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("ry",  &qplayer::Interactive::ry,
             py::arg("theta"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("rz",  &qplayer::Interactive::rz,
             py::arg("theta"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("u1",  &qplayer::Interactive::u1,
             py::arg("lam"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("u2",  &qplayer::Interactive::u2,
             py::arg("phi"), py::arg("lam"), py::arg("q"),
             py::return_value_policy::reference_internal)
        .def("u3",  &qplayer::Interactive::u3,
             py::arg("theta"), py::arg("phi"), py::arg("lam"), py::arg("q"),
             py::return_value_policy::reference_internal)

        // ── Two-qubit gates (chainable) ───────────────────────────────
        .def("cx",   &qplayer::Interactive::cx,
             py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cy",   &qplayer::Interactive::cy,
             py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("cz",   &qplayer::Interactive::cz,
             py::arg("ctrl"), py::arg("tgt"),
             py::return_value_policy::reference_internal)
        .def("swap", &qplayer::Interactive::swap,
             py::arg("q0"), py::arg("q1"),
             py::return_value_policy::reference_internal)

        // ── Three-qubit gate (chainable) ──────────────────────────────
        .def("ccx",  &qplayer::Interactive::ccx,
             py::arg("c0"), py::arg("c1"), py::arg("tgt"),
             py::return_value_policy::reference_internal)

        // ── Barrier (fusion boundary, no-op for state) ────────────────
        .def("barrier", &qplayer::Interactive::barrier,
             py::return_value_policy::reference_internal)

        // ── Measurement (immediate collapse) ──────────────────────────
        // measure(q) -> int: returns 0 or 1, no cubit write.
        // measure(q, cubit) -> None: accumulates into the bound Result.
        // measure_all() -> list[int]: one outcome per qubit.
        .def("measure",
             py::overload_cast<qplayer::QubitIndex>(
                 &qplayer::Interactive::measure),
             py::arg("q"))
        .def("measure",
             py::overload_cast<qplayer::QubitIndex, qplayer::ClassicalBitIndex>(
                 &qplayer::Interactive::measure),
             py::arg("q"), py::arg("cubit"))
        .def("measure_all", &qplayer::Interactive::measure_all)

        // ── State access ────────────────
        // get_statevector: post-collapse AoS StateVector -> numpy complex128.
        // get_statevector_raw: SoA view is unsafe across calls; omitted from
        // Python (note: raw view deferred).
        .def("get_statevector",
             [](const qplayer::Interactive& iv) {
                 return sv_to_numpy(iv.get_statevector());
             })
        // get_statevector_nonzero: sparse enumeration without densify.
        // Returns list[tuple[int, complex]] — StateIndex as arbitrary-precision int.
        .def("get_statevector_nonzero",
             [](const qplayer::Interactive& iv) {
                 return nonzero_amplitudes_to_py(iv.get_statevector_nonzero());
             },
             "Return nonzero (basis_index, amplitude) pairs in ascending index "
             "order without densifying. SparseState supports up to 128 qubits; "
             "TensorNetwork raises SimulatorError.")
        .def("probability",
             &qplayer::Interactive::probability,
             py::arg("q"), py::arg("outcome"))
        .def("expectation_value",
             [](const qplayer::Interactive& iv, const std::string& pauli) {
                 return iv.expectation_value(pauli);
             },
             py::arg("pauli"))

        // ── Accumulated result ─────────────────────────────────────────
        .def("result",
             &qplayer::Interactive::result,
             py::return_value_policy::copy)
        .def("clear_result", &qplayer::Interactive::clear_result)

        // ── Utility ────────────────────────────────────────────────────
        .def("num_qubits", &qplayer::Interactive::num_qubits)
        .def("is_active",  &qplayer::Interactive::is_active);

    // ── ChannelType enum (noise_model.hpp) ──────────────
    // Tag enum used for diagnostics / logging; backends always apply the Kraus
    // operator list regardless of this tag.
    py::enum_<qplayer::ChannelType>(m, "ChannelType")
        .value("Identity",        qplayer::ChannelType::Identity)
        .value("Depolarizing",    qplayer::ChannelType::Depolarizing)
        .value("AmplitudeDamping",qplayer::ChannelType::AmplitudeDamping)
        .value("PhaseDamping",    qplayer::ChannelType::PhaseDamping)
        .value("BitFlip",         qplayer::ChannelType::BitFlip)
        .value("PhaseFlip",       qplayer::ChannelType::PhaseFlip)
        .value("Readout",         qplayer::ChannelType::Readout)
        .value("Custom",          qplayer::ChannelType::Custom);

    // ── KrausChannel (noise_model.hpp) ─────────────────
    // Holds a list of Kraus operators (each is a list-of-list-of-complex matrix),
    // the target qubit index, and a ChannelType tag. pybind11 STL casters convert
    // UnitaryMatrix (vector<vector<complex<double>>>) to/from Python list-of-list.
    py::class_<qplayer::KrausChannel>(m, "KrausChannel")
        .def(py::init<>())
        .def_readwrite("operators",    &qplayer::KrausChannel::operators)
        .def_readwrite("target_qubit", &qplayer::KrausChannel::target_qubit)
        .def_readwrite("type",         &qplayer::KrausChannel::type);

    // ── GateProperty (device_model.hpp) ────────────────
    // Per-gate metadata: duration in nanoseconds and error rate (1 - fidelity).
    // Read-only from Python — callers obtain GateProperty from DeviceModel.
    py::class_<qplayer::GateProperty>(m, "GateProperty")
        .def_readonly("duration_ns", &qplayer::GateProperty::duration_ns)
        .def_readonly("error_rate",  &qplayer::GateProperty::error_rate);

    // ── NoiseModel (noise_model.hpp..) ───────────────
    // Gate-name-keyed Kraus channel registry. All add_* methods return NoiseModel&
    // for method chaining; reference_internal keeps the Python wrapper alive.
    // qubits default {} -> apply to every operand of the named gate (global rule).
    py::class_<qplayer::NoiseModel>(m, "NoiseModel")
        .def(py::init<>())

        // ── Channel registration (chainable) ──────────────────────────────
        .def("add_depolarizing",
             &qplayer::NoiseModel::add_depolarizing,
             py::arg("gate_name"), py::arg("p"),
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal,
             "Add depolarizing channel for gate_name with error probability p.")
        .def("add_amplitude_damping",
             &qplayer::NoiseModel::add_amplitude_damping,
             py::arg("gate_name"), py::arg("gamma"),
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal,
             "Add amplitude damping channel (T1 decay) with damping rate gamma.")
        .def("add_phase_damping",
             &qplayer::NoiseModel::add_phase_damping,
             py::arg("gate_name"), py::arg("lam"),
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal,
             "Add phase damping channel (T2 dephasing) with rate lam.")
        .def("add_bit_flip",
             &qplayer::NoiseModel::add_bit_flip,
             py::arg("gate_name"), py::arg("p"),
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal,
             "Add bit flip channel with probability p.")
        .def("add_phase_flip",
             &qplayer::NoiseModel::add_phase_flip,
             py::arg("gate_name"), py::arg("p"),
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal,
             "Add phase flip channel with probability p.")
        .def("add_readout_error",
             &qplayer::NoiseModel::add_readout_error,
             py::arg("qubit"), py::arg("p_meas_0_given_1"), py::arg("p_meas_1_given_0"),
             py::return_value_policy::reference_internal,
             "Add readout error for qubit. p_meas_0_given_1: P(0|1), p_meas_1_given_0: P(1|0).")
        .def("add_kraus",
             &qplayer::NoiseModel::add_kraus,
             py::arg("gate_name"), py::arg("channel"),
             py::arg("qubits") = std::vector<qplayer::QubitIndex>{},
             py::return_value_policy::reference_internal,
             "Add raw Kraus channel. Operators must satisfy trace-preserving condition.")

        // ── DeviceModel ingestion ─────────────────────────
        .def("load_device",
             &qplayer::NoiseModel::load_device,
             py::arg("device"),
             py::return_value_policy::reference_internal,
             "Synthesise T1/T2 relaxation Kraus channels from a DeviceModel.")

        // ── Query ─────────────────────────────────────────────────────────
        .def("has_channels_for_gate",
             &qplayer::NoiseModel::has_channels_for_gate,
             py::arg("gate_name"))
        .def("has_t1_t2", &qplayer::NoiseModel::has_t1_t2);

    // ── DeviceModel (device_model.hpp) ────────────
    // Hardware device characterisation: T1/T2 coherence, gate duration/error,
    // and the coupling map. Loaded from JSON via static factories.
    py::class_<qplayer::DeviceModel>(m, "DeviceModel")
        // ── Factories ─────────────────────────────────────────────────────
        .def_static("from_json_file",
                    &qplayer::DeviceModel::from_json_file,
                    py::arg("path"),
                    "Load device model from a JSON file (schema). Throws CircuitError on error.")
        .def_static("from_json_string",
                    &qplayer::DeviceModel::from_json_string,
                    py::arg("json_text"),
                    "Parse device model from a JSON string (schema). Throws CircuitError on error.")

        // ── Coherence times ────────────────────────────────────────────────
        .def("T1", &qplayer::DeviceModel::T1, py::arg("q"),
             "Return T1 coherence time in ns for qubit q (infinity if unknown).")
        .def("T2", &qplayer::DeviceModel::T2, py::arg("q"),
             "Return T2 coherence time in ns for qubit q (infinity if unknown).")

        // ── Gate properties ────────────────────────────────────────────────
        .def("gate_property",
             &qplayer::DeviceModel::gate_property,
             py::arg("gate_name"), py::arg("qubits"),
             "Return GateProperty (duration_ns, error_rate) for gate on given qubits.")
        .def("has_gate_property",
             &qplayer::DeviceModel::has_gate_property,
             py::arg("gate_name"), py::arg("qubits"))

        // ── Coupling map ───────────────────────────────────────────────────
        .def("has_coupling_map", &qplayer::DeviceModel::has_coupling_map,
             "Return True if a coupling map (restricted connectivity) is defined.")
        .def("coupling_map",
             [](const qplayer::DeviceModel& dm) {
                 // Return as list[tuple[int,int]] for natural Python access.
                 const auto& cm = dm.coupling_map();
                 py::list result;
                 for (const auto& [a, b] : cm) {
                     result.append(py::make_tuple(
                         static_cast<unsigned>(a), static_cast<unsigned>(b)));
                 }
                 return result;
             },
             "Return coupling map as list[tuple[int,int]]. Raises CircuitError if no coupling map.")
        .def("num_qubits", &qplayer::DeviceModel::num_qubits);

    // ── QRegister (sole user-facing entry point) ─────────
    // Batch API + Interactive facade + NoiseModel/DeviceModel injection.
    py::class_<qplayer::QRegister>(m, "QRegister")
        .def(py::init<qplayer::SimType, qplayer::ProcType>(),
             py::arg("sim_type") = qplayer::SimType::FullState,
             py::arg("proc")     = qplayer::ProcType::Auto)

        // ── Interactive facade (early expose) ─────
        // qr.interactive is the sole access point for Interactive-mode ops        
        // reference_internal: Interactive lifetime is tied to QRegister.
        .def_property_readonly("interactive",
             [](qplayer::QRegister& qr) -> qplayer::Interactive& {
                 return qr.interactive;
             },
             py::return_value_policy::reference_internal)

        // ── Input (parse) ──────────────────────────────────────────────
        .def("parse",      &qplayer::QRegister::parse,      py::arg("qasm_text"))
        .def("parse_file", &qplayer::QRegister::parse_file, py::arg("file_path"))

        // ── Compile ─────────────────────────────────────────
        // Default arg values match C++ header (optimization_level=1, device_json="").
        .def("transpile",  &qplayer::QRegister::transpile,
             py::arg("circuit"),
             py::arg("optimization_level") = 1,
             py::arg("device_json")        = "")

        // ── Batch run ───────────────────────────────────────
        // nshots uses uint32_t matching the C++ header.
        .def("run",        &qplayer::QRegister::run,
             py::arg("circuit"),
             py::arg("nshots")           = 1024,
             py::arg("enable_fusion")    = true,
             py::arg("fusion_max_width") = 4)
        .def("result",     &qplayer::QRegister::result,
             py::return_value_policy::copy)

        // ── State access ────────────────
        // get_statevector: AoS StateVector -> numpy complex128 ndarray.
        .def("get_statevector",
             [](const qplayer::QRegister& qr) {
                 return sv_to_numpy(qr.get_statevector());
             })
        // get_statevector_raw: SoA RawStateView -> (re: float64, im: float64)
        // tuple copy. Zero-copy buffer protocol view is Phase 11
        // (Phase 11 row).
        .def("get_statevector_raw",
             [](const qplayer::QRegister& qr) {
                 return raw_to_numpy_tuple(qr.get_statevector_raw());
             })
        // get_statevector_nonzero: sparse enumeration without densify.
        // Returns list[tuple[int, complex]] — StateIndex as arbitrary-precision int.
        // SparseState works up to 128 qubits; TensorNetwork raises SimulatorError.
        .def("get_statevector_nonzero",
             [](const qplayer::QRegister& qr) {
                 return nonzero_amplitudes_to_py(qr.get_statevector_nonzero());
             },
             "Return nonzero (basis_index, amplitude) pairs in ascending index "
             "order without densifying. SparseState supports up to 128 qubits; "
             "TensorNetwork raises SimulatorError.")
        // expectation_value: pauli="" defaults to all-Z.
        // Cast disambiguates from the Observable overload;
        // the Observable/Estimator/Sampler bindings land in.
        .def("expectation_value",
             static_cast<double (qplayer::QRegister::*)(std::string_view) const>(
                 &qplayer::QRegister::expectation_value),
             py::arg("pauli") = "")

        // ── Seed control ──────────────────────────
        .def("set_seed",     &qplayer::QRegister::set_seed,     py::arg("seed"))
        .def("current_seed", &qplayer::QRegister::current_seed)

        // ── Noise / device injection ───────
        // set_noise_model(): apply a pre-built NoiseModel to subsequent run() calls.
        .def("set_noise_model",
             &qplayer::QRegister::set_noise_model,
             py::arg("model"),
             "Inject a NoiseModel; subsequent run() calls apply Monte Carlo noise.")
        // load_device_model(): load device JSON -> DeviceModel + NoiseModel in one step.
        // Stores the DeviceModel for transpile() fallback (truth table).
        .def("load_device_model",
             [](qplayer::QRegister& qr, const std::string& json_path) {
                 qr.load_device_model(json_path);
             },
             py::arg("json_path"),
             "Load device JSON, store for transpile() fallback, and synthesise T1/T2 noise.")

        // ── Utilities ──────────────────────────────────────────────────
        .def("sim_type",   &qplayer::QRegister::sim_type)
        .def("max_qubits", &qplayer::QRegister::max_qubits)
        .def_static("gpu_available", &qplayer::QRegister::gpu_available);

    // ── Observable / Sampler / Estimator (Qiskit primitive) ──
    // Weighted Pauli-sum observable H = Sum(c_i * P_i). Real coeffs (Hermitian);
    // Pauli order is Qiskit big-endian, same as expectation_value.
    py::class_<qplayer::Observable>(m, "Observable")
        .def(py::init<>())
        // from_list takes Python [(pauli, coeff), ...] -> Observable.
        .def_static(
            "from_list",
            [](const std::vector<std::pair<std::string, double>>& terms) {
                qplayer::Observable obs;
                for (const auto& [pauli, coeff] : terms) obs.add_term(coeff, pauli);
                return obs;
            },
            py::arg("terms"),
            "Build from [(pauli, coeff), ...]; raises CircuitError on bad Pauli.")
        // add_term returns *this; reference_internal keeps Python chaining valid.
        .def(
            "add_term",
            [](qplayer::Observable& self, double coeff,
               const std::string& pauli) -> qplayer::Observable& {
                return self.add_term(coeff, pauli);
            },
            py::arg("coeff"), py::arg("pauli"),
            py::return_value_policy::reference_internal,
            "Append one (coeff, pauli) term; chainable.")
        .def("terms", &qplayer::Observable::terms,
             "List of (coeff, pauli) terms in insertion order.")
        .def("__len__", &qplayer::Observable::size);

    // Estimator: exact <H> via QRegister::run() + expectation_value(obs).
    // keep_alive<1,2>: the QRegister (arg 2) must outlive the Estimator (arg 1)
    // because the facade holds a QRegister&.
    py::class_<qplayer::Estimator>(m, "Estimator")
        .def(py::init<qplayer::QRegister&>(), py::arg("qr"), py::keep_alive<1, 2>())
        .def("run", &qplayer::Estimator::run,
             py::arg("circuit"), py::arg("observable"),
             "Compute <H> = Sum(c_i*<P_i>) exactly (no shot noise).");

    // Sampler: measurement counts via QRegister::run().counts().
    py::class_<qplayer::Sampler>(m, "Sampler")
        .def(py::init<qplayer::QRegister&>(), py::arg("qr"), py::keep_alive<1, 2>())
        .def("run", &qplayer::Sampler::run,
             py::arg("circuit"), py::arg("nshots") = 1024,
             "Sample the measurement distribution as a {bitstring: count} dict.");

    // Module version mirror: QPLAYER_VERSION_STRING is
    // injected by CMakeLists.txt target_compile_definitions so
    // qplayer.__version__ tracks the C++ source-of-truth.
#ifndef QPLAYER_VERSION_STRING
#  error "QPLAYER_VERSION_STRING was not defined by CMake; check target_compile_definitions for _qplayer"
#endif
    m.attr("__version__") = QPLAYER_VERSION_STRING;
}
