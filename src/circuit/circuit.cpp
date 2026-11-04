/// @file circuit.cpp
/// @brief -- Phase 2 implementation of the Circuit IR builder
///
///
/// Method bodies validate qubit ranges, classical-register name collisions,
/// opaque dimension / unitarity / trace-preservation, and the single-direction
/// opaque body promotion (DeclarationOnly -> UnitaryMatrix / KrausChannel).
/// Serialisation paths (to_qasm / to_json / from_json / load_json_file) stay
/// stubbed -- owns that work.

#include "circuit/circuit.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <utility>

#include "error.hpp"

// GCC 13 emits false-positive -Wmaybe-uninitialized warnings for the
// `push_back(<Alternative>{...})` form when -O2 inlines the variant
// constructor across translation units. The variant alternative is always
// fully initialised by its brace-init expression, but the optimiser loses
// track and walks the union's other arms. Suppress at file scope -- every
// builder in this TU follows the same pattern, and clang + GCC <= 12 do not
// report the issue (no real defect masked).
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace qplayer {

namespace {

constexpr const char* kModule       = "Circuit";
constexpr double      kUnitaryTol   = 1e-10;                    // /
constexpr double      kKrausTol     = 1e-10;                    // trace-preserving tolerance

// ── Qubit range validation ───────────────────────────────────────────────────
// Throws CircuitError when q >= num_qubits. Used by every gate builder and by
// MeasureOp / Reset to keep the IR free of dangling qubit references.
void check_qubit(QubitIndex q, QubitIndex num_qubits, const char* gate_name) {
    if (q >= num_qubits) {
        throw CircuitError{std::string{gate_name} + ": qubit " + std::to_string(q) +
                           " out of range (num_qubits=" + std::to_string(num_qubits) + ")"};
    }
}

// ── Single-qubit gate factory ────────────────────────────────────────────────
GateOp make_1q(GateType t, const char* name, QubitIndex q) {
    GateOp g;
    g.type = t;
    g.name = name;
    g.qubits.push_back(q);
    return g;
}

GateOp make_1q_params(GateType t, const char* name, QubitIndex q,
                      std::initializer_list<double> params) {
    GateOp g = make_1q(t, name, q);
    g.params.assign(params);
    return g;
}

// ── Two-qubit gate factory ───────────────────────────────────────────────────
GateOp make_2q(GateType t, const char* name, QubitIndex a, QubitIndex b) {
    GateOp g;
    g.type = t;
    g.name = name;
    g.qubits.push_back(a);
    g.qubits.push_back(b);
    return g;
}

GateOp make_2q_params(GateType t, const char* name, QubitIndex a, QubitIndex b,
                      std::initializer_list<double> params) {
    GateOp g = make_2q(t, name, a, b);
    g.params.assign(params);
    return g;
}

// ── std::array -> UnitaryMatrix conversion (define_opaque 1Q / 2Q overloads) ─
UnitaryMatrix array_to_unitary_1q(const std::array<std::array<std::complex<double>, 2>, 2>& m) {
    UnitaryMatrix out(2, std::vector<std::complex<double>>(2));
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 2; ++j) out[i][j] = m[i][j];
    }
    return out;
}

UnitaryMatrix array_to_unitary_2q(const std::array<std::array<std::complex<double>, 4>, 4>& m) {
    UnitaryMatrix out(4, std::vector<std::complex<double>>(4));
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) out[i][j] = m[i][j];
    }
    return out;
}

// ── Unitary verification: matrix is NxN with N == 2^num_qubits and U†U ≈ I ───
// Throws CircuitError on dimension mismatch, ragged rows, or unitarity failure.
// kMaxOpaqueQubits now lives in circuit/gate_op.hpp so the backend can bind it
// to its kernel bound with a static_assert. The rationale for the
// value is documented there; the O(2^3n) verification cost below is what caps it.
// Kraus operators share the same dimension law, so the same cap applies there.

void check_opaque_qubit_count(QubitIndex num_qubits, const std::string& opaque_name) {
    if (num_qubits == 0 || num_qubits > kMaxOpaqueQubits) {
        throw CircuitError{"define_opaque(\"" + opaque_name + "\"): num_qubits " +
                           std::to_string(num_qubits) +
                           " out of range (1.." +
                           std::to_string(kMaxOpaqueQubits) +
                           "); a dense 2^n x 2^n matrix is verified in O(2^3n)"};
    }
}

void verify_unitary(const UnitaryMatrix& m, QubitIndex num_qubits, const std::string& opaque_name) {
    check_opaque_qubit_count(num_qubits, opaque_name);
    const std::size_t expected = std::size_t{1} << num_qubits;
    if (m.size() != expected) {
        throw CircuitError{"define_opaque(\"" + opaque_name + "\"): matrix has " +
                           std::to_string(m.size()) + " rows, expected " +
                           std::to_string(expected) + " (2^" + std::to_string(num_qubits) + ")"};
    }
    for (std::size_t i = 0; i < expected; ++i) {
        if (m[i].size() != expected) {
            throw CircuitError{"define_opaque(\"" + opaque_name + "\"): row " +
                               std::to_string(i) + " has " + std::to_string(m[i].size()) +
                               " cols, expected " + std::to_string(expected)};
        }
    }
    // U†U[i][j] = sum_k conj(U[k][i]) * U[k][j]; expect delta_ij within tolerance.
    for (std::size_t i = 0; i < expected; ++i) {
        for (std::size_t j = 0; j < expected; ++j) {
            std::complex<double> acc{0.0, 0.0};
            for (std::size_t k = 0; k < expected; ++k) {
                acc += std::conj(m[k][i]) * m[k][j];
            }
            const std::complex<double> target = (i == j) ? std::complex<double>{1.0, 0.0}
                                                         : std::complex<double>{0.0, 0.0};
            if (std::abs(acc - target) > kUnitaryTol) {
                throw CircuitError{"define_opaque(\"" + opaque_name +
                                   "\"): matrix is not unitary (|U†U - I|[" +
                                   std::to_string(i) + "," + std::to_string(j) +
                                   "] > 1e-10)"};
            }
        }
    }
}

// ── Kraus trace-preserving verification: sum_i K_i†K_i ≈ I ───────────────────
void verify_kraus(const KrausChannel& ch, QubitIndex num_qubits, const std::string& opaque_name) {
    // Same dimension law as the unitary path: each operator is 2^n x 2^n.
    check_opaque_qubit_count(num_qubits, opaque_name);
    if (ch.operators.empty()) {
        throw CircuitError{"define_opaque(\"" + opaque_name +
                           "\"): KrausChannel has no operators"};
    }
    const std::size_t expected = std::size_t{1} << num_qubits;
    for (const auto& K : ch.operators) {
        if (K.size() != expected) {
            throw CircuitError{"define_opaque(\"" + opaque_name +
                               "\"): Kraus operator has " + std::to_string(K.size()) +
                               " rows, expected " + std::to_string(expected)};
        }
        for (const auto& row : K) {
            if (row.size() != expected) {
                throw CircuitError{"define_opaque(\"" + opaque_name +
                                   "\"): Kraus operator row has " +
                                   std::to_string(row.size()) +
                                   " cols, expected " + std::to_string(expected)};
            }
        }
    }
    // (sum K†K)[i][j] = sum_op sum_k conj(K_op[k][i]) * K_op[k][j]; expect delta_ij.
    for (std::size_t i = 0; i < expected; ++i) {
        for (std::size_t j = 0; j < expected; ++j) {
            std::complex<double> acc{0.0, 0.0};
            for (const auto& K : ch.operators) {
                for (std::size_t k = 0; k < expected; ++k) {
                    acc += std::conj(K[k][i]) * K[k][j];
                }
            }
            const std::complex<double> target = (i == j) ? std::complex<double>{1.0, 0.0}
                                                         : std::complex<double>{0.0, 0.0};
            if (std::abs(acc - target) > kKrausTol) {
                throw CircuitError{"define_opaque(\"" + opaque_name +
                                   "\"): Kraus channel is not trace-preserving "
                                   "(|sum K†K - I|[" + std::to_string(i) + "," +
                                   std::to_string(j) + "] > 1e-10)"};
            }
        }
    }
}

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────────
// : if the caller passes num_cubits > 0 we auto-create a register
// named "c" of that width at index 0. num_cubits == 0 leaves the register
// list empty -- users build their layout with add_creg() instead.
Circuit::Circuit(QubitIndex num_qubits, ClassicalBitIndex num_cubits)
    : num_qubits_(num_qubits) {
    if (num_cubits > 0) {
        classical_registers_.push_back(ClassicalRegister{"c", num_cubits, 0});
    }
}

// ── Single-qubit gates (qelib1.inc id, x, y, z, h, s, sdg, t, tdg) ───────────
Circuit& Circuit::id (QubitIndex q) { check_qubit(q, num_qubits_, "id");  ops_.push_back(make_1q(GateType::I,   "id",  q)); return *this; }
Circuit& Circuit::h  (QubitIndex q) { check_qubit(q, num_qubits_, "h");   ops_.push_back(make_1q(GateType::H,   "h",   q)); return *this; }
Circuit& Circuit::x  (QubitIndex q) { check_qubit(q, num_qubits_, "x");   ops_.push_back(make_1q(GateType::X,   "x",   q)); return *this; }
Circuit& Circuit::y  (QubitIndex q) { check_qubit(q, num_qubits_, "y");   ops_.push_back(make_1q(GateType::Y,   "y",   q)); return *this; }
Circuit& Circuit::z  (QubitIndex q) { check_qubit(q, num_qubits_, "z");   ops_.push_back(make_1q(GateType::Z,   "z",   q)); return *this; }
Circuit& Circuit::s  (QubitIndex q) { check_qubit(q, num_qubits_, "s");   ops_.push_back(make_1q(GateType::S,   "s",   q)); return *this; }
Circuit& Circuit::sdg(QubitIndex q) { check_qubit(q, num_qubits_, "sdg"); ops_.push_back(make_1q(GateType::Sdg, "sdg", q)); return *this; }
Circuit& Circuit::t  (QubitIndex q) { check_qubit(q, num_qubits_, "t");   ops_.push_back(make_1q(GateType::T,   "t",   q)); return *this; }
Circuit& Circuit::tdg(QubitIndex q) { check_qubit(q, num_qubits_, "tdg"); ops_.push_back(make_1q(GateType::Tdg, "tdg", q)); return *this; }
Circuit& Circuit::sx  (QubitIndex q) { check_qubit(q, num_qubits_, "sx");   ops_.push_back(make_1q(GateType::SX,   "sx",   q)); return *this; }
Circuit& Circuit::sxdg(QubitIndex q) { check_qubit(q, num_qubits_, "sxdg"); ops_.push_back(make_1q(GateType::SXdg, "sxdg", q)); return *this; }

// ── Single-qubit rotations / U gates (qelib1.inc rx, ry, rz, u1, u2, u3) ─────
Circuit& Circuit::rx(double theta, QubitIndex q) {
    check_qubit(q, num_qubits_, "rx");
    ops_.push_back(make_1q_params(GateType::RX, "rx", q, {theta}));
    return *this;
}
Circuit& Circuit::ry(double theta, QubitIndex q) {
    check_qubit(q, num_qubits_, "ry");
    ops_.push_back(make_1q_params(GateType::RY, "ry", q, {theta}));
    return *this;
}
Circuit& Circuit::rz(double theta, QubitIndex q) {
    check_qubit(q, num_qubits_, "rz");
    ops_.push_back(make_1q_params(GateType::RZ, "rz", q, {theta}));
    return *this;
}
Circuit& Circuit::u1(double lam, QubitIndex q) {
    check_qubit(q, num_qubits_, "u1");
    ops_.push_back(make_1q_params(GateType::U1, "u1", q, {lam}));
    return *this;
}
Circuit& Circuit::u2(double phi, double lam, QubitIndex q) {
    check_qubit(q, num_qubits_, "u2");
    ops_.push_back(make_1q_params(GateType::U2, "u2", q, {phi, lam}));
    return *this;
}
Circuit& Circuit::u3(double theta, double phi, double lam, QubitIndex q) {
    check_qubit(q, num_qubits_, "u3");
    ops_.push_back(make_1q_params(GateType::U3, "u3", q, {theta, phi, lam}));
    return *this;
}

// ── Two-qubit gates (qelib1.inc cx, cy, cz, ch, swap, crz, cu1, cu3) ─────────
// Distinct-qubit invariant: 2Q gates that act on the same qubit twice are
// physically nonsensical and would crash backend dispatchers, so we reject
// them at builder time.
namespace {
void check_distinct(QubitIndex a, QubitIndex b, const char* gate_name) {
    if (a == b) {
        throw CircuitError{std::string{gate_name} + ": qubits must be distinct (got " +
                           std::to_string(a) + ", " + std::to_string(b) + ")"};
    }
}
}  // namespace

Circuit& Circuit::cx(QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "cx"); check_qubit(tgt, num_qubits_, "cx"); check_distinct(ctrl, tgt, "cx");
    ops_.push_back(make_2q(GateType::CX, "cx", ctrl, tgt));
    return *this;
}
Circuit& Circuit::cy(QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "cy"); check_qubit(tgt, num_qubits_, "cy"); check_distinct(ctrl, tgt, "cy");
    ops_.push_back(make_2q(GateType::CY, "cy", ctrl, tgt));
    return *this;
}
Circuit& Circuit::cz(QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "cz"); check_qubit(tgt, num_qubits_, "cz"); check_distinct(ctrl, tgt, "cz");
    ops_.push_back(make_2q(GateType::CZ, "cz", ctrl, tgt));
    return *this;
}
Circuit& Circuit::ch(QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "ch"); check_qubit(tgt, num_qubits_, "ch"); check_distinct(ctrl, tgt, "ch");
    ops_.push_back(make_2q(GateType::CH, "ch", ctrl, tgt));
    return *this;
}
Circuit& Circuit::swap(QubitIndex q0, QubitIndex q1) {
    check_qubit(q0, num_qubits_, "swap"); check_qubit(q1, num_qubits_, "swap"); check_distinct(q0, q1, "swap");
    ops_.push_back(make_2q(GateType::SWAP, "swap", q0, q1));
    return *this;
}
Circuit& Circuit::crz(double theta, QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "crz"); check_qubit(tgt, num_qubits_, "crz"); check_distinct(ctrl, tgt, "crz");
    ops_.push_back(make_2q_params(GateType::CRZ, "crz", ctrl, tgt, {theta}));
    return *this;
}
Circuit& Circuit::cu1(double lam, QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "cu1"); check_qubit(tgt, num_qubits_, "cu1"); check_distinct(ctrl, tgt, "cu1");
    ops_.push_back(make_2q_params(GateType::CU1, "cu1", ctrl, tgt, {lam}));
    return *this;
}
Circuit& Circuit::cu3(double theta, double phi, double lam, QubitIndex ctrl, QubitIndex tgt) {
    check_qubit(ctrl, num_qubits_, "cu3"); check_qubit(tgt, num_qubits_, "cu3"); check_distinct(ctrl, tgt, "cu3");
    ops_.push_back(make_2q_params(GateType::CU3, "cu3", ctrl, tgt, {theta, phi, lam}));
    return *this;
}
// ECR is a genuine 4x4 gate; qubit order is significant (not symmetric), so
// (q0, q1) map to matrix operand bits 0 and 1 respectively.
Circuit& Circuit::ecr(QubitIndex q0, QubitIndex q1) {
    check_qubit(q0, num_qubits_, "ecr"); check_qubit(q1, num_qubits_, "ecr"); check_distinct(q0, q1, "ecr");
    ops_.push_back(make_2q(GateType::ECR, "ecr", q0, q1));
    return *this;
}

// Google native 2Q gates. Qubit order is significant for all three.
Circuit& Circuit::iswap(QubitIndex q0, QubitIndex q1) {
    check_qubit(q0, num_qubits_, "iswap"); check_qubit(q1, num_qubits_, "iswap"); check_distinct(q0, q1, "iswap");
    ops_.push_back(make_2q(GateType::ISWAP, "iswap", q0, q1));
    return *this;
}
Circuit& Circuit::sqrt_iswap(QubitIndex q0, QubitIndex q1) {
    check_qubit(q0, num_qubits_, "sqrt_iswap"); check_qubit(q1, num_qubits_, "sqrt_iswap"); check_distinct(q0, q1, "sqrt_iswap");
    ops_.push_back(make_2q(GateType::SQRT_ISWAP, "sqrt_iswap", q0, q1));
    return *this;
}
// fSim(theta, phi): params[0]=theta, params[1]=phi.
Circuit& Circuit::fsim(double theta, double phi, QubitIndex q0, QubitIndex q1) {
    check_qubit(q0, num_qubits_, "fsim"); check_qubit(q1, num_qubits_, "fsim"); check_distinct(q0, q1, "fsim");
    ops_.push_back(make_2q_params(GateType::FSIM, "fsim", q0, q1, {theta, phi}));
    return *this;
}
// sycamore = fSim(pi/2, pi/6) — Google Sycamore processor native gate.
Circuit& Circuit::sycamore(QubitIndex q0, QubitIndex q1) {
    return fsim(M_PI / 2.0, M_PI / 6.0, q0, q1);
}

// ── Three-qubit gates (qelib1.inc ccx Toffoli, cswap Fredkin) ────────────────
Circuit& Circuit::ccx(QubitIndex c0, QubitIndex c1, QubitIndex tgt) {
    check_qubit(c0,  num_qubits_, "ccx");
    check_qubit(c1,  num_qubits_, "ccx");
    check_qubit(tgt, num_qubits_, "ccx");
    if (c0 == c1 || c0 == tgt || c1 == tgt) {
        throw CircuitError{"ccx: qubits must be distinct (got " +
                           std::to_string(c0) + ", " + std::to_string(c1) + ", " +
                           std::to_string(tgt) + ")"};
    }
    GateOp g;
    g.type = GateType::CCX;
    g.name = "ccx";
    g.qubits = {c0, c1, tgt};
    ops_.push_back(std::move(g));
    return *this;
}

Circuit& Circuit::cswap(QubitIndex ctrl, QubitIndex t0, QubitIndex t1) {
    check_qubit(ctrl, num_qubits_, "cswap");
    check_qubit(t0,   num_qubits_, "cswap");
    check_qubit(t1,   num_qubits_, "cswap");
    if (ctrl == t0 || ctrl == t1 || t0 == t1) {
        throw CircuitError{"cswap: qubits must be distinct (got " +
                           std::to_string(ctrl) + ", " + std::to_string(t0) + ", " +
                           std::to_string(t1) + ")"};
    }
    GateOp g;
    g.type = GateType::CSwap;
    g.name = "cswap";
    g.qubits = {ctrl, t0, t1};
    ops_.push_back(std::move(g));
    return *this;
}

// ── Control flow ─────────────────────────────────────────────────────────────
Circuit& Circuit::measure(QubitIndex q, ClassicalBitIndex cubit) {
    check_qubit(q, num_qubits_, "measure");
    const ClassicalBitIndex total = total_cubits();
    if (cubit >= total) {
        throw CircuitError{"measure: cubit " + std::to_string(cubit) +
                           " out of range (total_cubits=" + std::to_string(total) + ")"};
    }
    ops_.push_back(MeasureOp{q, cubit});
    return *this;
}

// measure_all maps logical qubit i -> cubit i. + :
// the default "c" register width must be at least num_qubits for this to be
// well-defined.
Circuit& Circuit::measure_all() {
    if (total_cubits() < num_qubits_) {
        throw CircuitError{"measure_all: total_cubits=" +
                           std::to_string(total_cubits()) +
                           " < num_qubits=" + std::to_string(num_qubits_) +
                           " (no default register or register too small)"};
    }
    for (QubitIndex q = 0; q < num_qubits_; ++q) {
        ops_.push_back(MeasureOp{q, static_cast<ClassicalBitIndex>(q)});
    }
    return *this;
}

Circuit& Circuit::barrier(std::initializer_list<QubitIndex> qubits) {
    BarrierOp b;
    if (qubits.size() > 0) {
        b.qubits.reserve(qubits.size());
        for (QubitIndex q : qubits) {
            check_qubit(q, num_qubits_, "barrier");
            b.qubits.push_back(q);
        }
    }
    // Empty list is preserved as "all qubits" -- frontend section 5.1.
    ops_.push_back(std::move(b));
    return *this;
}

Circuit& Circuit::reset(QubitIndex q) {
    check_qubit(q, num_qubits_, "reset");
    // Reset rides on GateOp{type=Reset} so the Op variant stays at 5 members
    // (gate_op.hpp header note +).
    ops_.push_back(make_1q(GateType::Reset, "reset", q));
    return *this;
}

Circuit& Circuit::if_then(ClassicalRegisterIndex creg,
                          std::uint64_t          value,
                          const GateOp&          gate) {
    if (creg >= classical_registers_.size()) {
        throw CircuitError{"if_then: creg index " + std::to_string(creg) +
                           " out of range (classical_registers=" +
                           std::to_string(classical_registers_.size()) + ")"};
    }
    // A comparison value the register cannot represent makes the branch
    // permanently dead. The QASM front end already rejects this
    // (SemanticValidatorPass, "exceeds maximum"), but the C++ API accepted it
    // silently — so the same typo was an error through one entry point and a
    // no-op through the other (audit #23; the report described the QASM path as
    // unchecked, which it is not). Treat both the same way.
    const ClassicalBitIndex width = classical_registers_[creg].width;
    if (width < 64) {
        const std::uint64_t max_value = (std::uint64_t{1} << width) - 1;
        if (value > max_value) {
            throw CircuitError{"if_then: condition value " + std::to_string(value) +
                               " exceeds maximum " + std::to_string(max_value) +
                               " for creg \"" + classical_registers_[creg].name +
                               "\" of width " + std::to_string(width)};
        }
    }
    // Validate the body gate's qubit references too, so a malformed body
    // cannot slip through if_then() and surface at backend dispatch.
    for (QubitIndex q : gate.qubits) {
        check_qubit(q, num_qubits_, "if_then(body)");
    }
    ops_.push_back(IfOp{creg, value, gate});
    return *this;
}

// ── Classical register management ─────────────────────────────────
// add_creg appends a new register, assigning offset = sum of previous widths
// so MeasureOp.cubit can index a flat global cubit space across registers.
ClassicalRegisterIndex Circuit::add_creg(const std::string& name,
                                         ClassicalBitIndex  width) {
    if (width == 0) {
        throw CircuitError{"add_creg(\"" + name + "\"): width must be > 0"};
    }
    for (const auto& cr : classical_registers_) {
        if (cr.name == name) {
            throw CircuitError{"add_creg(\"" + name + "\"): duplicate register name"};
        }
    }
    const ClassicalBitIndex offset = total_cubits();
    classical_registers_.push_back(ClassicalRegister{name, width, offset});
    return static_cast<ClassicalRegisterIndex>(classical_registers_.size() - 1);
}

ClassicalRegisterIndex Circuit::creg(const std::string& name) const {
    for (std::size_t i = 0; i < classical_registers_.size(); ++i) {
        if (classical_registers_[i].name == name) {
            return static_cast<ClassicalRegisterIndex>(i);
        }
    }
    throw CircuitError{"creg(\"" + name + "\"): register name not found"};
}

const std::vector<ClassicalRegister>& Circuit::classical_registers() const noexcept {
    return classical_registers_;
}

ClassicalBitIndex Circuit::total_cubits() const noexcept {
    ClassicalBitIndex total = 0;
    for (const auto& cr : classical_registers_) total += cr.width;
    return total;
}

// ── Expectation-value targets ────────────────────────────

Circuit& Circuit::add_expectation(std::string_view pauli) {
    if (pauli.empty()) {
        throw CircuitError{"add_expectation: Pauli string must not be empty"};
    }
    if (pauli.size() != static_cast<std::size_t>(num_qubits_)) {
        throw CircuitError{"add_expectation: Pauli string length (" +
                           std::to_string(pauli.size()) + ") != num_qubits (" +
                           std::to_string(num_qubits_) + ")"};
    }
    for (char ch : pauli) {
        if (ch != 'I' && ch != 'X' && ch != 'Y' && ch != 'Z') {
            throw CircuitError{std::string("add_expectation: invalid Pauli char '") +
                               ch + "' (only I/X/Y/Z allowed)"};
        }
    }
    // Idempotent: registering the same target twice has no extra effect.
    std::string p{pauli};
    if (std::find(expectation_targets_.begin(), expectation_targets_.end(), p) ==
        expectation_targets_.end()) {
        expectation_targets_.push_back(std::move(p));
    }
    return *this;
}

const std::vector<std::string>& Circuit::expectation_targets() const noexcept {
    return expectation_targets_;
}

// ── Opaque definitions (+) ─────────────────────────────────
// Lookup helper: returns the index into opaque_definitions_, or size() when
// the name is unknown. Used by every declare/define/has/get path.
namespace {
std::size_t find_opaque(const std::vector<OpaqueDefinition>& defs,
                        const std::string&                   name) {
    for (std::size_t i = 0; i < defs.size(); ++i) {
        if (defs[i].name == name) return i;
    }
    return defs.size();
}
}  // namespace

Circuit& Circuit::declare_opaque(const std::string& name, QubitIndex num_qubits) {
    if (name.empty()) {
        throw CircuitError{"declare_opaque: name must be non-empty"};
    }
    if (num_qubits == 0) {
        throw CircuitError{"declare_opaque(\"" + name + "\"): num_qubits must be > 0"};
    }
    if (find_opaque(opaque_definitions_, name) != opaque_definitions_.size()) {
        throw CircuitError{"declare_opaque(\"" + name + "\"): duplicate opaque name"};
    }
    opaque_definitions_.push_back(OpaqueDefinition{name, num_qubits, DeclarationOnly{}});
    return *this;
}

// opaque() — append an opaque CALL. Field layout must match what
// CircuitBuilder emits for a QASM opaque call so both paths hand the backend an
// identical GateOp (circuit_builder.cpp buildGateCall).
Circuit& Circuit::opaque(const std::string& name, std::initializer_list<QubitIndex> qubits) {
    if (name.empty()) {
        throw CircuitError{"opaque: name must be non-empty"};
    }
    if (qubits.size() == 0) {
        throw CircuitError{"opaque(\"" + name + "\"): needs at least one qubit"};
    }
    for (QubitIndex q : qubits) {
        check_qubit(q, num_qubits_, "opaque");
    }
    // Distinct operands: a definition matrix indexes one local bit per operand,
    // so repeating a qubit has no meaning.
    std::vector<QubitIndex> seen(qubits.begin(), qubits.end());
    std::sort(seen.begin(), seen.end());
    if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
        throw CircuitError{"opaque(\"" + name + "\"): qubits must be distinct"};
    }
    // Arity is checked only when a definition is already registered; an
    // undefined name stays legal here and is rejected by SemanticValidatorPass.
    const std::size_t idx = find_opaque(opaque_definitions_, name);
    if (idx != opaque_definitions_.size()) {
        const QubitIndex declared = opaque_definitions_[idx].num_qubits;
        if (static_cast<std::size_t>(declared) != qubits.size()) {
            throw CircuitError{"opaque(\"" + name + "\"): definition takes " +
                               std::to_string(declared) + " qubits, called with " +
                               std::to_string(qubits.size())};
        }
    }

    GateOp g;
    g.type        = GateType::Opaque;
    g.name        = name;
    g.opaque_name = name;
    g.qubits.assign(qubits.begin(), qubits.end());
    g.is_opaque   = true;
    ops_.push_back(std::move(g));
    return *this;
}

namespace {

// Promote an existing OpaqueDefinition.body from DeclarationOnly to a concrete
// variant. single-direction promotion: any concrete -> anything else
// is rejected (no demotion, no Unitary <-> Kraus swap).
void promote_body(OpaqueDefinition&                                              def,
                  std::variant<DeclarationOnly, UnitaryMatrix, KrausChannel>&&   new_body,
                  const std::string&                                              name) {
    if (!std::holds_alternative<DeclarationOnly>(def.body)) {
        throw CircuitError{"define_opaque(\"" + name +
                           "\"): opaque already defined (single-direction promotion)"};
    }
    def.body = std::move(new_body);
}

// Insert a fresh OpaqueDefinition with a concrete body (caller skipped
// declare_opaque). Same name-collision rule applies.
void insert_with_body(std::vector<OpaqueDefinition>&                                  defs,
                      const std::string&                                              name,
                      QubitIndex                                                       num_qubits,
                      std::variant<DeclarationOnly, UnitaryMatrix, KrausChannel>&&    body) {
    defs.push_back(OpaqueDefinition{name, num_qubits, std::move(body)});
}

}  // namespace

Circuit& Circuit::define_opaque(const std::string& name,
                                const std::array<std::array<std::complex<double>, 2>, 2>& matrix) {
    if (name.empty()) {
        throw CircuitError{"define_opaque: name must be non-empty"};
    }
    UnitaryMatrix u = array_to_unitary_1q(matrix);
    verify_unitary(u, /*num_qubits=*/1, name);

    const std::size_t idx = find_opaque(opaque_definitions_, name);
    if (idx == opaque_definitions_.size()) {
        insert_with_body(opaque_definitions_, name, /*num_qubits=*/1, std::move(u));
    } else {
        if (opaque_definitions_[idx].num_qubits != 1) {
            throw CircuitError{"define_opaque(\"" + name + "\"): declared num_qubits=" +
                               std::to_string(opaque_definitions_[idx].num_qubits) +
                               " but 2x2 overload expects 1"};
        }
        promote_body(opaque_definitions_[idx], std::move(u), name);
    }
    return *this;
}

Circuit& Circuit::define_opaque(const std::string& name,
                                const std::array<std::array<std::complex<double>, 4>, 4>& matrix) {
    if (name.empty()) {
        throw CircuitError{"define_opaque: name must be non-empty"};
    }
    UnitaryMatrix u = array_to_unitary_2q(matrix);
    verify_unitary(u, /*num_qubits=*/2, name);

    const std::size_t idx = find_opaque(opaque_definitions_, name);
    if (idx == opaque_definitions_.size()) {
        insert_with_body(opaque_definitions_, name, /*num_qubits=*/2, std::move(u));
    } else {
        if (opaque_definitions_[idx].num_qubits != 2) {
            throw CircuitError{"define_opaque(\"" + name + "\"): declared num_qubits=" +
                               std::to_string(opaque_definitions_[idx].num_qubits) +
                               " but 4x4 overload expects 2"};
        }
        promote_body(opaque_definitions_[idx], std::move(u), name);
    }
    return *this;
}

Circuit& Circuit::define_opaque(const std::string&   name,
                                QubitIndex           num_qubits,
                                const UnitaryMatrix& matrix) {
    if (name.empty()) {
        throw CircuitError{"define_opaque: name must be non-empty"};
    }
    if (num_qubits == 0) {
        throw CircuitError{"define_opaque(\"" + name + "\"): num_qubits must be > 0"};
    }
    UnitaryMatrix u = matrix;  // verified copy; verify_unitary requires fixed contents
    verify_unitary(u, num_qubits, name);

    const std::size_t idx = find_opaque(opaque_definitions_, name);
    if (idx == opaque_definitions_.size()) {
        insert_with_body(opaque_definitions_, name, num_qubits, std::move(u));
    } else {
        if (opaque_definitions_[idx].num_qubits != num_qubits) {
            throw CircuitError{"define_opaque(\"" + name + "\"): declared num_qubits=" +
                               std::to_string(opaque_definitions_[idx].num_qubits) +
                               " but argument has num_qubits=" + std::to_string(num_qubits)};
        }
        promote_body(opaque_definitions_[idx], std::move(u), name);
    }
    return *this;
}

Circuit& Circuit::define_opaque(const std::string&  name,
                                const KrausChannel& channel) {
    if (name.empty()) {
        throw CircuitError{"define_opaque: name must be non-empty"};
    }
    // Kraus overload needs to know num_qubits, which we infer from the first
    // operator's row count or from a prior declare_opaque entry. We pick the
    // declared value when present so the user gets a clear dimension-mismatch
    // diagnostic if the channel disagrees with the declaration.
    const std::size_t idx = find_opaque(opaque_definitions_, name);
    QubitIndex inferred_n = 0;
    if (idx != opaque_definitions_.size()) {
        inferred_n = opaque_definitions_[idx].num_qubits;
    } else {
        if (channel.operators.empty()) {
            throw CircuitError{"define_opaque(\"" + name +
                               "\"): KrausChannel has no operators (cannot infer num_qubits)"};
        }
        const std::size_t rows = channel.operators.front().size();
        if (rows == 0 || (rows & (rows - 1)) != 0) {
            throw CircuitError{"define_opaque(\"" + name +
                               "\"): Kraus operator dimension " + std::to_string(rows) +
                               " is not a power of 2"};
        }
        // Compute log2(rows).
        QubitIndex n = 0;
        for (std::size_t r = rows; r > 1; r >>= 1) ++n;
        inferred_n = n;
    }
    verify_kraus(channel, inferred_n, name);

    if (idx == opaque_definitions_.size()) {
        insert_with_body(opaque_definitions_, name, inferred_n, KrausChannel{channel});
    } else {
        promote_body(opaque_definitions_[idx], KrausChannel{channel}, name);
    }
    return *this;
}

bool Circuit::has_opaque_definition(const std::string& name) const {
    return find_opaque(opaque_definitions_, name) != opaque_definitions_.size();
}

const OpaqueDefinition& Circuit::opaque_definition(const std::string& name) const {
    const std::size_t idx = find_opaque(opaque_definitions_, name);
    if (idx == opaque_definitions_.size()) {
        throw CircuitError{"opaque_definition(\"" + name + "\"): name not found"};
    }
    return opaque_definitions_[idx];
}

const std::vector<OpaqueDefinition>& Circuit::opaque_definitions() const noexcept {
    return opaque_definitions_;
}

// ── Queries ──────────────────────────────────────────────────────────────────
QubitIndex             Circuit::num_qubits() const noexcept { return num_qubits_; }
ClassicalBitIndex      Circuit::num_cubits() const noexcept { return total_cubits(); }
std::size_t            Circuit::num_ops()    const noexcept { return ops_.size(); }
const std::vector<Op>& Circuit::ops()        const noexcept { return ops_; }
const CircuitMetadata& Circuit::metadata()   const noexcept { return metadata_; }

// has_classical_control: true when the circuit needs the section 12.4 per-shot
// path. Triggers:
//   1. Any IfOp present.
//   2. A MeasureOp followed by a non-Measure / non-Barrier op on the same
//      qubit -- mid-circuit measurement with downstream gates.
bool Circuit::has_classical_control() const {
    // Single forward pass with a per-qubit "already measured" bitmap. The former
    // shape re-scanned every following op for each MeasureOp — O(ops^2) — and
    // QRegister::run() plus all three backends' execute() call this on every run
    // (audit #28). Cost is now linear in the total operand count.
    //
    // Semantics are unchanged: an IfOp anywhere, or a GateOp/IdleOp touching a
    // qubit that an earlier MeasureOp already collapsed, means the circuit needs
    // the per-shot path. BarrierOp and a repeated MeasureOp do not reactivate a
    // qubit, so they are simply not consulted.
    std::vector<bool> measured(static_cast<std::size_t>(num_qubits_), false);
    const auto is_measured = [&](QubitIndex q) {
        return q < num_qubits_ && measured[static_cast<std::size_t>(q)];
    };

    for (const auto& op : ops_) {
        if (std::holds_alternative<IfOp>(op)) {
            return true;
        }
        if (const auto* m = std::get_if<MeasureOp>(&op)) {
            if (m->qubit < num_qubits_) {
                measured[static_cast<std::size_t>(m->qubit)] = true;
            }
            continue;
        }
        if (const auto* g = std::get_if<GateOp>(&op)) {
            for (QubitIndex q : g->qubits) {
                if (is_measured(q)) return true;
            }
            continue;
        }
        if (const auto* idle = std::get_if<IdleOp>(&op)) {
            if (is_measured(idle->qubit)) return true;
        }
        // BarrierOp: no effect on the measured set.
    }
    return false;
}

// requires_per_shot: the routing question, as opposed to has_classical_control()'s
// literal one. Adds one trigger — a Reset on a qubit some gate has
// already touched.
//
// A leading `reset q[i];` on an untouched qubit is deterministic (the state is
// already |0>), which is why the touched bitmap is worth keeping rather than
// treating every Reset as a trigger: that idiom opens a great many QASM files
// and would otherwise cost every one of them an nshots-fold slowdown.
//
// MeasureOp deliberately does NOT mark a qubit touched here: `measure q; reset q`
// is already caught by has_classical_control()'s second trigger, and marking it
// would only duplicate that.
bool Circuit::requires_per_shot() const {
    if (has_classical_control()) return true;

    std::vector<bool> touched(static_cast<std::size_t>(num_qubits_), false);
    for (const auto& op : ops_) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            if (g->type == GateType::Reset) {
                for (QubitIndex q : g->qubits) {
                    if (q < num_qubits_ && touched[static_cast<std::size_t>(q)]) return true;
                }
            }
            for (QubitIndex q : g->qubits) {
                if (q < num_qubits_) touched[static_cast<std::size_t>(q)] = true;
            }
            continue;
        }
        if (const auto* idle = std::get_if<IdleOp>(&op)) {
            if (idle->qubit < num_qubits_) {
                touched[static_cast<std::size_t>(idle->qubit)] = true;
            }
        }
        // MeasureOp / BarrierOp / IfOp: see the note above; IfOp already
        // returned true through has_classical_control().
    }
    return false;
}

// ── Serialisation ─────────────────────────────────────────────────────
// Member entry points delegate to the free functions in serializer.cpp /
// qasm_emitter.cpp so this translation unit stays focused on Circuit
// invariants. The free functions are declared in circuit.hpp.

std::string Circuit::to_qasm() const { return emit_qasm_v2(*this); }
std::string Circuit::to_json() const { return serialize_circuit_json(*this); }

std::string Circuit::draw(CircuitDrawStyle style) const {
    return draw_circuit_text(*this, style);  //
}

Circuit Circuit::from_json(std::string_view text) {
    return deserialize_circuit_json(text);
}

Circuit Circuit::load_json_file(const std::string& path) {
    return load_circuit_json_file(path);
}

// ── Internal payload constructor (friend of Circuit) ────────────────────────
// Materialises a Circuit with the exact internal state recovered from a JSON
// dump. Public builders cannot reproduce every detail (e.g. GateOp with both
// duration_ns and noise_channels populated, an Op stream starting from a
// post-transpile state), so the serialiser hands the parsed payload through
// this single bridge instead. Validation already ran during JSON decode +
// during the staged replay below where possible -- this bridge does not
// re-run unitary / Kraus checks because those would fail for legitimate
// transpile() outputs (e.g. fused unitaries that no longer match the source
// gate's tolerance budget).
Circuit detail_make_circuit_from_payload(QubitIndex                       num_qubits,
                                         std::vector<ClassicalRegister>   regs,
                                         std::vector<OpaqueDefinition>    opaques,
                                         std::vector<Op>                  ops,
                                         std::vector<std::string>         expectation_targets,
                                         CircuitMetadata                  meta) {
    Circuit c{num_qubits, /*num_cubits=*/0};
    c.classical_registers_ = std::move(regs);
    c.opaque_definitions_  = std::move(opaques);
    c.ops_                 = std::move(ops);
    c.expectation_targets_ = std::move(expectation_targets);
    c.metadata_            = std::move(meta);
    return c;
}

// Internal bridge for CircuitBuilder: appends a pre-built Op to the
// circuit's op stream without going through the public builder API. Used when
// CircuitBuilder needs to emit a BarrierOp (from a std::vector<QubitIndex>
// rather than an initializer_list) or an Opaque GateOp. Not exposed to users.
void detail_circuit_push_op(Circuit& c, Op op) {
    c.ops_.push_back(std::move(op));
}

}  // namespace qplayer

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic pop
#endif
