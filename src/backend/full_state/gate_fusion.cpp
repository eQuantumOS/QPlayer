/// @file gate_fusion.cpp
/// @brief Runtime Gate Fusion implementation.
///
/// Greedy-window algorithm: consecutive unitary GateOps whose qubit union fits
/// within max_width are accumulated into one FusedGate via matrix product.
/// Boundary ops (Measure/Barrier/If/Idle) and non-unitary GateOps flush the
/// window and pass through unchanged.
///
/// Matrix conventions (consistent with full_state_cpu.cpp apply_gate):
/// - State index bit ordering: bit q of index i = (i >> q) & 1.
/// - Local index inside a k-qubit subspace: qubits are sorted ascending;
/// the lowest-indexed qubit contributes bit 0 of the local index.
/// - FusedGate.matrix is row-major: element [r][c] = matrix[r*(1<<k) + c].
/// - Product order: M_fused = M_last * ... * M_first  (later gates left-multiply).

#include "backend/full_state/gate_fusion.hpp"
#include "backend/full_state/cpu_dispatcher.hpp"  // select_for_host
#include "backend/full_state/gate_matrix.hpp"     // ecr/iswap/sqrt_iswap/fsim matrix data
#include "error.hpp"
#include "log.hpp"

#include <algorithm>    // std::sort, std::set_union, std::find
#include <cassert>
#include <cmath>
#include <complex>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace qplayer {
namespace detail {

// ── Internal helpers ──────────────────────────────────────────────────────────

// Complex alias for brevity.
using Cx = std::complex<double>;

// Return the full 2^k x 2^k unitary matrix for a GateOp acting on k qubits.
// The returned matrix uses the same bit convention as the state vector:
// local index bit b corresponds to qubits[b] (qubits sorted ascending).
//
// For a 1-qubit gate the matrix is the 2x2 sub-unitary (same as gate_matrix
// in full_state_cpu.cpp). For controlled gates the full block matrix is
// constructed explicitly: I (lower left/right blocks) + U (upper 2x2 block
// when all control bits are 1). For SWAP/CSwap a permutation matrix is built.
//
// The local-bit↔qubit mapping must match the state-vector indexing used by
// apply_fused_gate so that the fused matrix is composed consistently.

std::vector<Cx> gate_full_matrix(const GateOp& op) {
    const GateType type = op.type;
    const auto& qubits  = op.qubits;
    const auto& params  = op.params;

    // Derive 2x2 target sub-unitary for 1Q and controlled gates.
    // Mirrors gate_matrix() in full_state_cpu.cpp (same param order).
    auto make_1q = [&](GateType gt) -> std::vector<Cx> {
        const double INV_SQRT2 = M_SQRT1_2;
        switch (gt) {
            case GateType::I:   return {{1,0},{0,0},{0,0},{1,0}};
            case GateType::X:   return {{0,0},{1,0},{1,0},{0,0}};
            case GateType::Y:   return {{0,0},{0,-1},{0,1},{0,0}};
            case GateType::Z:   return {{1,0},{0,0},{0,0},{-1,0}};
            case GateType::H:   return {{INV_SQRT2,0},{INV_SQRT2,0},
                                        {INV_SQRT2,0},{-INV_SQRT2,0}};
            case GateType::S:   return {{1,0},{0,0},{0,0},{0,1}};
            case GateType::Sdg: return {{1,0},{0,0},{0,0},{0,-1}};
            case GateType::T:   return {{1,0},{0,0},{0,0},{INV_SQRT2,INV_SQRT2}};
            case GateType::Tdg: return {{1,0},{0,0},{0,0},{INV_SQRT2,-INV_SQRT2}};
            // SX = sqrt(X), SXdg = SX dagger.
            case GateType::SX:   return {{0.5,0.5},{0.5,-0.5},{0.5,-0.5},{0.5,0.5}};
            case GateType::SXdg: return {{0.5,-0.5},{0.5,0.5},{0.5,0.5},{0.5,-0.5}};
            case GateType::RX: {
                double c = std::cos(params[0]*0.5), s = std::sin(params[0]*0.5);
                return {{c,0},{0,-s},{0,-s},{c,0}};
            }
            case GateType::RY: {
                double c = std::cos(params[0]*0.5), s = std::sin(params[0]*0.5);
                return {{c,0},{-s,0},{s,0},{c,0}};
            }
            case GateType::RZ: {
                double c = std::cos(params[0]*0.5), s = std::sin(params[0]*0.5);
                return {{c,-s},{0,0},{0,0},{c,s}};
            }
            case GateType::U1: {
                double l = params[0];
                return {{1,0},{0,0},{0,0},{std::cos(l),std::sin(l)}};
            }
            case GateType::U2: {
                double phi = params[0], lam = params[1];
                double cp = std::cos(phi), sp = std::sin(phi);
                double cl = std::cos(lam), sl = std::sin(lam);
                double cpl = std::cos(phi+lam), spl = std::sin(phi+lam);
                return {{INV_SQRT2,0},{-INV_SQRT2*cl,-INV_SQRT2*sl},
                        {INV_SQRT2*cp,INV_SQRT2*sp},{INV_SQRT2*cpl,INV_SQRT2*spl}};
            }
            case GateType::U3: {
                double th = params[0], phi = params[1], lam = params[2];
                double ct = std::cos(th*0.5), st = std::sin(th*0.5);
                double cp = std::cos(phi), sp = std::sin(phi);
                double cl = std::cos(lam), sl = std::sin(lam);
                double cpl = std::cos(phi+lam), spl = std::sin(phi+lam);
                return {{ct,0},{-cl*st,-sl*st},
                        {cp*st,sp*st},{cpl*ct,spl*ct}};
            }
            // Controlled gate target sub-unitaries.
            case GateType::CX:  return {{0,0},{1,0},{1,0},{0,0}};
            case GateType::CY:  return {{0,0},{0,-1},{0,1},{0,0}};
            case GateType::CZ:  return {{1,0},{0,0},{0,0},{-1,0}};
            case GateType::CH:  return {{INV_SQRT2,0},{INV_SQRT2,0},
                                        {INV_SQRT2,0},{-INV_SQRT2,0}};
            case GateType::CRZ: {
                double c = std::cos(params[0]*0.5), s = std::sin(params[0]*0.5);
                return {{c,-s},{0,0},{0,0},{c,s}};
            }
            case GateType::CU1: {
                double l = params[0];
                return {{1,0},{0,0},{0,0},{std::cos(l),std::sin(l)}};
            }
            case GateType::CU3: {
                double th = params[0], phi = params[1], lam = params[2];
                double ct = std::cos(th*0.5), st = std::sin(th*0.5);
                double cp = std::cos(phi), sp = std::sin(phi);
                double cl = std::cos(lam), sl = std::sin(lam);
                double cpl = std::cos(phi+lam), spl = std::sin(phi+lam);
                return {{ct,0},{-cl*st,-sl*st},
                        {cp*st,sp*st},{cpl*ct,spl*ct}};
            }
            case GateType::CCX: return {{0,0},{1,0},{1,0},{0,0}};
            default:
                return {{1,0},{0,0},{0,0},{1,0}};  // identity fallback
        }
    };

    // Compute position of qubit q within sorted qubit list (0-based local bit).
    auto local_bit = [&](QubitIndex q) -> int {
        for (int b = 0; b < static_cast<int>(qubits.size()); ++b) {
            if (qubits[static_cast<std::size_t>(b)] == q) return b;
        }
        throw SimulatorError{"FullState", "fusion: qubit not found in local bit lookup"};
    };

    const int k = static_cast<int>(qubits.size());
    const int dim = 1 << k;  // local Hilbert-space dimension

    // ── 1-qubit gates ────────────────────────────────────────────────
    if (type == GateType::I || type == GateType::X || type == GateType::Y ||
        type == GateType::Z || type == GateType::H || type == GateType::S ||
        type == GateType::Sdg || type == GateType::T || type == GateType::Tdg ||
        type == GateType::SX || type == GateType::SXdg ||             //
        type == GateType::RX || type == GateType::RY || type == GateType::RZ ||
        type == GateType::U1 || type == GateType::U2 || type == GateType::U3) {
        // Single qubit: matrix is exactly the 2x2 sub-unitary (k==1 guaranteed).
        return make_1q(type);
    }

    // ── SWAP (2-qubit permutation) ───────────────────────────────────
    // SWAP between qubits[0] and qubits[1]. Local indices: bit lb0 ↔ bit lb1.
    // M[output][input] = 1 when output = swap(input).
    // Iterate over input col c; compute swapped output row r.
    if (type == GateType::SWAP) {
        const int lb0 = local_bit(qubits[0]);
        const int lb1 = local_bit(qubits[1]);
        std::vector<Cx> M(static_cast<std::size_t>(dim * dim), Cx{0,0});
        for (int c = 0; c < dim; ++c) {
            // Swap bit lb0 and bit lb1 in c to get the output row r.
            int b0 = (c >> lb0) & 1;
            int b1 = (c >> lb1) & 1;
            int r  = c;
            if (b0 != b1) {
                r ^= (1 << lb0);
                r ^= (1 << lb1);
            }
            M[static_cast<std::size_t>(r * dim + c)] = Cx{1, 0};
        }
        return M;
    }

    // ── ECR (genuine 2-qubit 4x4) ─────────────────────────
    // ecr_matrix_data() is already in operand-order local bits (bit 0 =
    // qubits[0], bit 1 = qubits[1]), which matches this function's local-bit
    // convention exactly, so it copies straight in. No identity fallback —
    // ECR must fuse as its true 4x4 or the noise/basis path is silently wrong.
    if (type == GateType::ECR) {
        const auto& d = ecr_matrix_data();  // 32 doubles = 16 (re,im), row-major
        std::vector<Cx> M(static_cast<std::size_t>(dim * dim), Cx{0, 0});
        for (int idx = 0; idx < dim * dim; ++idx) {
            M[static_cast<std::size_t>(idx)] =
                Cx{d[static_cast<std::size_t>(2 * idx)],
                   d[static_cast<std::size_t>(2 * idx + 1)]};
        }
        return M;
    }

    // ── Google native 2Q gates (genuine 4x4) ───────────────
    // Same copy convention as ECR above: operand-order local bits, row-major.
    // fSim is parametric — compute matrix from op.params before copying.
    auto copy_32d = [&](const double* d) {
        std::vector<Cx> M(static_cast<std::size_t>(dim * dim), Cx{0, 0});
        for (int idx = 0; idx < dim * dim; ++idx) {
            M[static_cast<std::size_t>(idx)] =
                Cx{d[static_cast<std::size_t>(2 * idx)],
                   d[static_cast<std::size_t>(2 * idx + 1)]};
        }
        return M;
    };
    if (type == GateType::ISWAP) {
        return copy_32d(iswap_matrix_data().data());
    }
    if (type == GateType::SQRT_ISWAP) {
        return copy_32d(sqrt_iswap_matrix_data().data());
    }
    if (type == GateType::FSIM) {
        // params[0]=theta, params[1]=phi.
        const auto m = fsim_matrix_data(op.params[0], op.params[1]);
        return copy_32d(m.data());
    }

    // ── Controlled-1Q gates (CX/CY/CZ/CH/CRZ/CU1/CU3) ──────────────
    // Convention: qubits[0]=control, qubits[1]=target (matches full_state_cpu.cpp).
    // Matrix = I on (control=0) subspace, U on (control=1) subspace.
    // Local bit assignment: bit lb_ctrl for control qubit, bit lb_tgt for target qubit.
    //
    // M[r][c]: output index r, input index c.
    // For the controlled region (ctrl=1): iterate input col c, not output row r,
    // to correctly place U elements.  For each input c with ctrl=1:
    //   input_tgt  = (c >> lb_tgt) & 1
    //   output row = (c & ~ctrl_bits) | (tgt_out << lb_tgt) with tgt_out chosen
    //   M[output_row][c] = U[tgt_out][input_tgt]
    // This matches the kernel: amp[out] = sum_in U[out_tgt][in_tgt] * amp[in].
    if (type == GateType::CX  || type == GateType::CY  || type == GateType::CZ ||
        type == GateType::CH  || type == GateType::CRZ || type == GateType::CU1 ||
        type == GateType::CU3) {
        const std::vector<Cx> U2 = make_1q(type);
        const int lb_ctrl = local_bit(qubits[0]);
        const int lb_tgt  = local_bit(qubits[1]);
        std::vector<Cx> M(static_cast<std::size_t>(dim * dim), Cx{0,0});
        for (int c = 0; c < dim; ++c) {
            const int ctrl_bit_c = (c >> lb_ctrl) & 1;
            const int tgt_bit_c  = (c >> lb_tgt)  & 1;
            if (ctrl_bit_c == 0) {
                // Identity block: row = col.
                M[static_cast<std::size_t>(c * dim + c)] = Cx{1, 0};
            } else {
                // U applied to target qubit: M[r][c] = U[tgt_out][tgt_bit_c].
                // Output row r has the same ctrl bits and extra bits as c,
                // with lb_tgt set to tgt_out.
                for (int tgt_out = 0; tgt_out < 2; ++tgt_out) {
                    int r = (c & ~(1 << lb_tgt)) | (tgt_out << lb_tgt);
                    M[static_cast<std::size_t>(r * dim + c)] +=
                        U2[static_cast<std::size_t>(tgt_out * 2 + tgt_bit_c)];
                }
            }
        }
        return M;
    }

    // ── CCX (Toffoli): qubits[0]+[1] control, qubits[2] target ──────
    if (type == GateType::CCX) {
        const std::vector<Cx> U2 = make_1q(GateType::X);
        const int lb_c0  = local_bit(qubits[0]);
        const int lb_c1  = local_bit(qubits[1]);
        const int lb_tgt = local_bit(qubits[2]);
        std::vector<Cx> M(static_cast<std::size_t>(dim * dim), Cx{0,0});
        for (int c = 0; c < dim; ++c) {
            const int c0_bit  = (c >> lb_c0) & 1;
            const int c1_bit  = (c >> lb_c1) & 1;
            const int tgt_bit_c = (c >> lb_tgt) & 1;
            if (c0_bit == 0 || c1_bit == 0) {
                // Identity block.
                M[static_cast<std::size_t>(c * dim + c)] = Cx{1, 0};
            } else {
                // Both controls active: apply X to target.
                // M[r][c] = U[tgt_out][tgt_bit_c].
                for (int tgt_out = 0; tgt_out < 2; ++tgt_out) {
                    int r = (c & ~(1 << lb_tgt)) | (tgt_out << lb_tgt);
                    M[static_cast<std::size_t>(r * dim + c)] +=
                        U2[static_cast<std::size_t>(tgt_out * 2 + tgt_bit_c)];
                }
            }
        }
        return M;
    }

    // ── CSwap (Fredkin): qubits[0] control, qubits[1]↔qubits[2] swap ─
    // For a swap gate M[r][c] is 1 when r = swap(c), i.e., M[output][input].
    // Iterate over input col c, compute swapped output row r.
    if (type == GateType::CSwap) {
        const int lb_ctrl = local_bit(qubits[0]);
        const int lb_s0   = local_bit(qubits[1]);
        const int lb_s1   = local_bit(qubits[2]);
        std::vector<Cx> M(static_cast<std::size_t>(dim * dim), Cx{0,0});
        for (int c = 0; c < dim; ++c) {
            const int ctrl_bit = (c >> lb_ctrl) & 1;
            if (ctrl_bit == 0) {
                // Identity: output = input.
                M[static_cast<std::size_t>(c * dim + c)] = Cx{1, 0};
            } else {
                // Swap lb_s0 and lb_s1 bits of c to get r.
                int b0 = (c >> lb_s0) & 1;
                int b1 = (c >> lb_s1) & 1;
                int r  = c;
                if (b0 != b1) {
                    r ^= (1 << lb_s0);
                    r ^= (1 << lb_s1);
                }
                M[static_cast<std::size_t>(r * dim + c)] = Cx{1, 0};
            }
        }
        return M;
    }

    // Reset or Opaque: caller should not reach here (fuse_circuit guards first).
    throw SimulatorError{"FullState",
        "fusion: gate_full_matrix called for non-unitary gate type"};
}

// Compute union of two sorted qubit vectors (result is also sorted).
static std::vector<QubitIndex> qubit_union(
        const std::vector<QubitIndex>& a,
        const std::vector<QubitIndex>& b) {
    std::vector<QubitIndex> out;
    out.reserve(a.size() + b.size());
    std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                   std::back_inserter(out));
    return out;
}

// Compute the identity matrix of dimension dim x dim.
static std::vector<Cx> identity_matrix(int dim) {
    std::vector<Cx> I(static_cast<std::size_t>(dim * dim), Cx{0, 0});
    for (int i = 0; i < dim; ++i) {
        I[static_cast<std::size_t>(i * dim + i)] = Cx{1, 0};
    }
    return I;
}

// Embed a gate's local matrix (defined on gate_qubits) into a larger subspace
// (defined on union_qubits, which is a superset). The embedding expands the
// matrix dimension from 2^|gate_qubits| to 2^|union_qubits| by tensoring
// identity on the extra qubits.
//
// For each pair of local indices (r_union, c_union) in the larger space:
//   The gate acts on its subset of qubits; for bits corresponding to gate
//   qubits, extract the local gate row/col; for extra qubits, the matrix is
//   diagonal (identity), so we only contribute when extra bits match.
static std::vector<Cx> embed_matrix(
        const std::vector<Cx>&        gate_mat,
        const std::vector<QubitIndex>& gate_qubits,
        const std::vector<QubitIndex>& union_qubits) {
    const int k_gate  = static_cast<int>(gate_qubits.size());
    const int k_union = static_cast<int>(union_qubits.size());
    const int dim_gate  = 1 << k_gate;
    const int dim_union = 1 << k_union;

    // Map: for each bit position in union_qubits, find if it is a gate qubit
    // and at which local bit in gate_qubits it lives.
    // bit_map[b_union] = b_gate (if union_qubits[b_union] is in gate_qubits)
    //                  = -1     (if it is an extra/identity qubit)
    std::vector<int> bit_map(static_cast<std::size_t>(k_union), -1);
    for (int b_union = 0; b_union < k_union; ++b_union) {
        QubitIndex q = union_qubits[static_cast<std::size_t>(b_union)];
        for (int b_gate = 0; b_gate < k_gate; ++b_gate) {
            if (gate_qubits[static_cast<std::size_t>(b_gate)] == q) {
                bit_map[static_cast<std::size_t>(b_union)] = b_gate;
                break;
            }
        }
    }

    std::vector<Cx> M(static_cast<std::size_t>(dim_union * dim_union), Cx{0,0});
    for (int r = 0; r < dim_union; ++r) {
        for (int c = 0; c < dim_union; ++c) {
            // Check that all extra (identity) qubit bits match between row and col.
            bool extra_match = true;
            for (int b = 0; b < k_union; ++b) {
                if (bit_map[static_cast<std::size_t>(b)] == -1) {
                    // Extra qubit: must be the same bit in row and col (identity).
                    if (((r >> b) & 1) != ((c >> b) & 1)) {
                        extra_match = false;
                        break;
                    }
                }
            }
            if (!extra_match) continue;

            // Compute the gate-local row and col indices from the union indices.
            int r_gate = 0, c_gate = 0;
            for (int b = 0; b < k_union; ++b) {
                int bg = bit_map[static_cast<std::size_t>(b)];
                if (bg >= 0) {
                    r_gate |= (((r >> b) & 1) << bg);
                    c_gate |= (((c >> b) & 1) << bg);
                }
            }
            M[static_cast<std::size_t>(r * dim_union + c)] =
                gate_mat[static_cast<std::size_t>(r_gate * dim_gate + c_gate)];
        }
    }
    return M;
}

// Left-multiply the window accumulator by one gate, in place.
//
// The dense route (embed the gate into a dim x dim matrix, then matmul) costs
// O(dim^3) = O(2^3k) per gate. But a gate touches only g <= 3 of the window's k
// qubits, so all it does is mix 2^g rows at a time — cost O(dim^2 * 2^g), which
// is 2^(k-g) times cheaper (8x for a 1Q gate in a width-4 window).
//
// That factor is not academic: fusing a 3149-gate circuit at width 4 spent more
// host time composing matrices than the device spent running them (q13_gcm ran
// 8.7 ms unfused against 26.8 ms fused before this change), because compose cost
// scales with gate count while the saving scales with state size.
//
// gate_qubits carries the gate's ORIGINAL operand order, matching the local bit
// order of gate_full_matrix() (bit j of the gate index selects gate_qubits[j]).
// window_qubits is sorted ascending and gives the window-local bit positions.
static void left_apply_gate(std::vector<Cx>&               M_acc,
                            const std::vector<Cx>&         gate_mat,
                            const std::vector<QubitIndex>& gate_qubits,
                            const std::vector<QubitIndex>& window_qubits) {
    const int k    = static_cast<int>(window_qubits.size());
    const int dim  = 1 << k;
    const int g    = static_cast<int>(gate_qubits.size());
    const int gdim = 1 << g;

    // Window-local bit position of each gate operand.
    int pos[3] = {0, 0, 0};   // g <= 3 for every fusable gate type
    int fixed_mask = 0;
    for (int j = 0; j < g; ++j) {
        const auto it = std::find(window_qubits.begin(), window_qubits.end(),
                                  gate_qubits[static_cast<std::size_t>(j)]);
        pos[j] = static_cast<int>(std::distance(window_qubits.begin(), it));
        fixed_mask |= 1 << pos[j];
    }

    // Scatter a gate-local index across the window-local bit positions.
    const auto spread = [&](int i) {
        int s = 0;
        for (int j = 0; j < g; ++j) {
            if ((i >> j) & 1) s |= 1 << pos[j];
        }
        return s;
    };

    std::vector<int> row_off(static_cast<std::size_t>(gdim));
    for (int i = 0; i < gdim; ++i) row_off[static_cast<std::size_t>(i)] = spread(i);

    Cx x[8];   // gdim <= 8
    for (int rb = 0; rb < dim; ++rb) {
        if (rb & fixed_mask) continue;          // enumerate row groups once
        for (int c = 0; c < dim; ++c) {
            for (int i = 0; i < gdim; ++i) {
                x[i] = M_acc[static_cast<std::size_t>(
                    (rb | row_off[static_cast<std::size_t>(i)]) * dim + c)];
            }
            for (int i = 0; i < gdim; ++i) {
                Cx sum{0, 0};
                for (int j = 0; j < gdim; ++j) {
                    sum += gate_mat[static_cast<std::size_t>(i * gdim + j)] * x[j];
                }
                M_acc[static_cast<std::size_t>(
                    (rb | row_off[static_cast<std::size_t>(i)]) * dim + c)] = sum;
            }
        }
    }
}

// Dense matrix-matrix product: C = A * B, all dim x dim.
static std::vector<Cx> matmul(const std::vector<Cx>& A,
                               const std::vector<Cx>& B,
                               int dim) {
    std::vector<Cx> C(static_cast<std::size_t>(dim * dim), Cx{0,0});
    for (int r = 0; r < dim; ++r) {
        for (int c = 0; c < dim; ++c) {
            Cx sum{0, 0};
            for (int i = 0; i < dim; ++i) {
                sum += A[static_cast<std::size_t>(r * dim + i)] *
                       B[static_cast<std::size_t>(i * dim + c)];
            }
            C[static_cast<std::size_t>(r * dim + c)] = sum;
        }
    }
    return C;
}

// ── Cost model ─────────────────────────────────────────────────────
// Weight of running one gate on its dedicated kernel, in 1Q-full-pass units.
//
// apply_gate() reduces most types to apply_controlled_1q. With c control bits
// that kernel still *iterates* dim/2 indices — the control mask only skips the
// body — but it only *touches* 2^-c of the amplitudes. So the cost splits into a
// fixed scan half and a subspace-proportional half:
//
//     w(c) = 0.5 + 0.5 * 2^-c        1Q 1.000 | 1 ctrl 0.750 | 2 ctrl 0.625
//
// q26 measurement (120 gates, mid-range qubits, net of the allocation floor,
// median of 7): h 1.04, rz 0.97, cx 0.75, cz 1.13, ccx 0.54 — the formula tracks
// the trend. The naive 2^-c (pure traffic) is wrong here: it predicts cx 0.5 and
// ccx 0.25, understating both because the scan is not vectorised (see the
// control branch of apply_controlled_1q) and pays for indices it then skips.
//
// Gates dispatched to apply_dense_2q_kernel (ECR, the Google natives) and all 1Q
// gates sweep the whole state, hence 1.0. Swap/CSwap never reach a window
// (flushes them first), so they need no weight here.
double gate_kernel_weight(const GateOp& g) {
    switch (g.type) {
        case GateType::CX:
        case GateType::CY:
        case GateType::CZ:
        case GateType::CH:
        case GateType::CRZ:
        case GateType::CU1:
        case GateType::CU3:
            return 0.75;   // one control

        case GateType::CCX:
            return 0.625;  // two controls

        default:
            return 1.0;    // no control
    }
}

// ── fuse_circuit ──────────────────────────────────────────────────────────────

std::vector<FusedOp> fuse_circuit(const std::vector<Op>& ops,
                                   std::uint32_t          max_width,
                                   QubitIndex             num_qubits,
                                   const FusionCostModel& cost) {
    // The GEMV kernels hold a local block of (1 << k) amplitudes in fixed stack
    // scratch, so k must stay within kMaxFusionWidth. Nothing on the way here
    // checked the user-supplied width (QRegister::run -> set_fusion ->
    // fusion_max_width_), and a width >= 9 wrote past the scratch: real stack
    // corruption, observed as "stack smashing detected" abort (audit #1). Clamp
    // rather than throw — enable_fusion/fusion_max_width are run() defaults, so a
    // throw would turn a tuning mistake into a failed simulation. Widths above 8
    // are useless anyway: the window matmul is O((2^k)^3).
    if (max_width > kMaxFusionWidth) {
        log::warn("fuse_circuit: fusion_max_width " + std::to_string(max_width) +
                  " exceeds the supported maximum " +
                  std::to_string(kMaxFusionWidth) + "; clamping to " +
                  std::to_string(kMaxFusionWidth));
        max_width = kMaxFusionWidth;
    }

    std::vector<FusedOp> result;
    result.reserve(ops.size());

    // Window state.
    std::vector<QubitIndex>              window_qubits;  // sorted union of accumulated qubits
    std::vector<std::vector<Cx>>         window_mats;    // one full matrix per accumulated gate
    std::vector<std::vector<QubitIndex>> window_gate_qubits; // per-gate qubit list (sorted)
    std::vector<GateOp>                  window_gates;   // originals, for the pass-through path
    double                               window_weight = 0.0;  // sum of gate_kernel_weight
    // sum of 2^arity(g): the gate-count-dependent half of the host composition
    // cost. The 2^(2k) half waits for flush, when the width is final.
    double                               window_arity_units = 0.0;

    // Flush the current window: compose all accumulated gate matrices into a
    // single FusedGate on window_qubits and push to result — but only when the
    // window pays for itself. Deciding here rather than during
    // accumulation is what makes the check sound: the final width, and thus
    // C(k), is only known once the window is closed.
    auto flush = [&]() {
        if (window_mats.empty()) return;

        const int k   = static_cast<int>(window_qubits.size());
        const int dim = 1 << k;

        // Not worth fusing: emit the originals so the backend runs each on its
        // dedicated kernel. Controlled gates land here when too few of them (or
        // too little 1Q work) share a window to outweigh a whole-state GEMV
        // sweep — absorbing them would widen their access from a 2^-c subspace
        // to the entire state vector. Small states land here too: composing the
        // window costs host time that fusion cannot earn back when a pass over
        // the state is cheap — and passing through skips the
        // composition, so the verdict avoids that cost rather than paying it.
        const std::uint32_t kw = static_cast<std::uint32_t>(k);
        const double threshold = cost.window_cost(kw)
                               + cost.compose_cost(kw, window_arity_units, num_qubits);
        if (window_weight <= threshold) {
            for (GateOp& g : window_gates) result.emplace_back(std::move(g));
            window_qubits.clear();
            window_mats.clear();
            window_gate_qubits.clear();
            window_gates.clear();
            window_weight = 0.0;
            window_arity_units = 0.0;
            return;
        }

        // Compose: M_fused = M_last * ... * M_first, applying each gate in place
        // (later gate = left factor). left_apply_gate() mixes only the 2^g rows
        // the gate touches, so this is O(dim^2 * 2^g) per gate rather than the
        // O(dim^3) of embed-then-matmul.
        std::vector<Cx> M_acc = identity_matrix(dim);
        for (std::size_t gi = 0; gi < window_mats.size(); ++gi) {
            left_apply_gate(M_acc, window_mats[gi], window_gate_qubits[gi],
                            window_qubits);
        }

        FusedGate fg;
        fg.qubits = window_qubits;
        fg.matrix = std::move(M_acc);
        result.emplace_back(std::move(fg));

        window_qubits.clear();
        window_mats.clear();
        window_gate_qubits.clear();
        window_gates.clear();
        window_weight = 0.0;
        window_arity_units = 0.0;
    };

    for (const Op& op : ops) {
        std::visit([&](auto&& node) {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, GateOp>) {
                const GateOp& g = node;

                // Non-unitary gate types: flush window then throw.
                if (g.type == GateType::Reset || g.type == GateType::Opaque) {
                    flush();
                    throw SimulatorError{"FullState",
                        "fusion: Reset/Opaque not fusable (+)"};
                }

                // Permutation gates: Swap/CSwap collapse to a sparse
                // permutation matrix that, when fused, runs as a dense 8x8/16x16
                // GEMV (only 1 non-zero per row -> >=87.5% wasted FMAs). The
                // dedicated apply_swap_kernel does a branch-filtered amplitude
                // exchange with no matmul, which is far cheaper. So flush the
                // window and pass the gate through; the backend dispatches it to
                // the swap kernel. Order is preserved (flush emits the prior block
                // first, then this gate).
                if (g.type == GateType::SWAP || g.type == GateType::CSwap) {
                    flush();
                    result.emplace_back(g);
                    return;
                }

                // Sorted qubit set used only for union computation (window_qubits must
                // remain sorted for qubit_union() to work correctly).  The original
                // qubit order from g.qubits must be preserved in window_gate_qubits so
                // that embed_matrix interprets the local-bit axes of gate_full_matrix(g)
                // correctly: gate_full_matrix builds local bit b as g.qubits[b] (original
                // order), so passing g_qubits_sorted to embed_matrix would swap ctrl/tgt
                // whenever qubit indices are not already ascending.  Fix: always push the
                // ORIGINAL g.qubits into window_gate_qubits; the union remains sorted.
                std::vector<QubitIndex> g_qubits_sorted = g.qubits;
                std::sort(g_qubits_sorted.begin(), g_qubits_sorted.end());

                // Compute candidate union with current window.
                std::vector<QubitIndex> candidate = qubit_union(window_qubits, g_qubits_sorted);

                if (candidate.size() > max_width) {
                    // Adding this gate would exceed max_width: flush first.
                    flush();
                    // After flush, start a new window with just this gate.
                    // window_qubits is the sorted union (here = sorted g.qubits);
                    // window_gate_qubits stores ORIGINAL qubit order for embed_matrix.
                    window_qubits     = std::move(g_qubits_sorted);
                    window_mats.push_back(gate_full_matrix(g));
                    window_gate_qubits.push_back(g.qubits);  // original order
                    window_gates.push_back(g);
                    window_weight = gate_kernel_weight(g);
                    window_arity_units = std::ldexp(1.0, static_cast<int>(g.qubits.size()));
                } else {
                    // Accumulate into the current window.
                    window_qubits = std::move(candidate);
                    window_mats.push_back(gate_full_matrix(g));
                    window_gate_qubits.push_back(g.qubits);  // original order
                    window_gates.push_back(g);
                    window_weight += gate_kernel_weight(g);
                    window_arity_units += std::ldexp(1.0, static_cast<int>(g.qubits.size()));
                }

            } else if constexpr (std::is_same_v<T, MeasureOp>) {
                flush();
                result.emplace_back(node);
            } else if constexpr (std::is_same_v<T, BarrierOp>) {
                flush();
                result.emplace_back(node);
            } else if constexpr (std::is_same_v<T, IfOp>) {
                flush();
                result.emplace_back(node);
            } else if constexpr (std::is_same_v<T, IdleOp>) {
                flush();
                result.emplace_back(node);
            }
        }, op);
    }

    // Flush any remaining window at end of circuit.
    flush();
    return result;
}

// ── Vectorised fused dense GEMV block loop ────────────────────────
// Per base block: gather 2^k amplitudes, run a dense complex matrix-vector
// multiply, scatter back.  The inner GEMV is reorganised from a row-wise dot
// product (scalar reduction) into a column-broadcast accumulate over rows:
//   out[r] += M[r][c] * amp[c]   for each column c
// With the matrix stored transposed (matT[c*ld + r] = M[r][c]) the r-loop is
// stride-1 contiguous, so it vectorises under #pragma omp simd with std::fma
// (FMA3). amp/out scratch is alignas(64); matT is loaded unaligned.
// AVX2 and AVX-512 variants differ only in the target attribute and the simd
// alignment width (coding-rules, performance-rules, single binary
// select_for_host). The OpenMP parallel-for over base blocks
// lives inside each worker so the kernel pointer is selected once, not per block.
//
// amp/out are sized kFusedScratchDim = 1 << kMaxFusionWidth, and fuse_circuit()
// clamps max_width to kMaxFusionWidth so local_dim can never exceed it (audit #1:
// an unchecked fusion_max_width >= 9 smashed the stack here). fusion_max_width
// defaults to 4 (local_dim 16) on the runtime path.

// Stack scratch dimension for one local amplitude block. Tied to the public cap
// so the two cannot drift apart.
constexpr int kFusedScratchDim = 1 << kMaxFusionWidth;

using FusedLoopFn = void (*)(double*, double*, int, int, std::int64_t,
                             const int*, const StateIndex*,
                             const double*, const double*);

__attribute__((target("avx2,fma")))
static void fused_gemv_loop_avx2(
    double* re, double* im, int local_dim, int n_free, std::int64_t n_base_i,
    const int* free_bits, const StateIndex* gate_contrib,
    const double* matT_re, const double* matT_im) {
#pragma omp parallel for schedule(static)
    for (std::int64_t base_ctr = 0; base_ctr < n_base_i; ++base_ctr) {
        // Reconstruct the base full-index by inserting zeros at gate qubit bits.
        StateIndex base_idx = 0;
        for (int fb = 0; fb < n_free; ++fb) {
            if ((base_ctr >> fb) & 1) {
                base_idx |= StateIndex{1} << free_bits[fb];
            }
        }

        alignas(64) double amp_re[kFusedScratchDim];
        alignas(64) double amp_im[kFusedScratchDim];
        alignas(64) double out_re[kFusedScratchDim];
        alignas(64) double out_im[kFusedScratchDim];
        for (int p = 0; p < local_dim; ++p) {
            const StateIndex idx = base_idx | gate_contrib[p];
            amp_re[p] = re[idx];
            amp_im[p] = im[idx];
            out_re[p] = 0.0;
            out_im[p] = 0.0;
        }

        // out[r] = sum_c matT[c][r] * amp[c]; accumulate column by column so the
        // r-loop is contiguous and vectorisable. FMA3 via std::fma.
        for (int c = 0; c < local_dim; ++c) {
            const double ar = amp_re[c];
            const double ai = amp_im[c];
            const int col_off = c * local_dim;
#pragma omp simd aligned(out_re, out_im : 32)
            for (int r = 0; r < local_dim; ++r) {
                const double mr = matT_re[col_off + r];
                const double mi = matT_im[col_off + r];
                out_re[r] = std::fma(mr, ar, std::fma(-mi, ai, out_re[r]));
                out_im[r] = std::fma(mr, ai, std::fma( mi, ar, out_im[r]));
            }
        }

        for (int p = 0; p < local_dim; ++p) {
            const StateIndex idx = base_idx | gate_contrib[p];
            re[idx] = out_re[p];
            im[idx] = out_im[p];
        }
    }
}

__attribute__((target("avx512f")))
static void fused_gemv_loop_avx512(
    double* re, double* im, int local_dim, int n_free, std::int64_t n_base_i,
    const int* free_bits, const StateIndex* gate_contrib,
    const double* matT_re, const double* matT_im) {
#pragma omp parallel for schedule(static)
    for (std::int64_t base_ctr = 0; base_ctr < n_base_i; ++base_ctr) {
        StateIndex base_idx = 0;
        for (int fb = 0; fb < n_free; ++fb) {
            if ((base_ctr >> fb) & 1) {
                base_idx |= StateIndex{1} << free_bits[fb];
            }
        }

        alignas(64) double amp_re[kFusedScratchDim];
        alignas(64) double amp_im[kFusedScratchDim];
        alignas(64) double out_re[kFusedScratchDim];
        alignas(64) double out_im[kFusedScratchDim];
        for (int p = 0; p < local_dim; ++p) {
            const StateIndex idx = base_idx | gate_contrib[p];
            amp_re[p] = re[idx];
            amp_im[p] = im[idx];
            out_re[p] = 0.0;
            out_im[p] = 0.0;
        }

        for (int c = 0; c < local_dim; ++c) {
            const double ar = amp_re[c];
            const double ai = amp_im[c];
            const int col_off = c * local_dim;
#pragma omp simd aligned(out_re, out_im : 64)
            for (int r = 0; r < local_dim; ++r) {
                const double mr = matT_re[col_off + r];
                const double mi = matT_im[col_off + r];
                out_re[r] = std::fma(mr, ar, std::fma(-mi, ai, out_re[r]));
                out_im[r] = std::fma(mr, ai, std::fma( mi, ar, out_im[r]));
            }
        }

        for (int p = 0; p < local_dim; ++p) {
            const StateIndex idx = base_idx | gate_contrib[p];
            re[idx] = out_re[p];
            im[idx] = out_im[p];
        }
    }
}

// Select the fused-GEMV loop kernel once per process (CPUID dispatch).
static FusedLoopFn fused_loop_kernel() {
    static FusedLoopFn fn = qplayer::detail::select_for_host(
        &fused_gemv_loop_avx2, &fused_gemv_loop_avx512);
    return fn;
}

// ── apply_fused_gate ─────────────────────────────────────────────────────────

void apply_fused_gate(double* re, double* im, StateIndex dim,
                      const FusedGate& g) {
    const int k         = static_cast<int>(g.qubits.size());
    const int local_dim = 1 << k;  // 2^k

    // Total number of "base" indices (all qubits outside g.qubits fixed).
    // Each base corresponds to one dense matrix-vector multiply on 2^k amplitudes.
    const StateIndex n_base = dim >> k;  // 2^(n-k)

    // Precompute: for each local pattern p (0..2^k-1), the bit contribution
    // that the g.qubits bits make to the full state index.
    // full_index = base_index (bits at non-gate positions) | gate_contribution(p)
    // gate_contribution(p) = sum over b in [0,k): ((p >> b) & 1) << g.qubits[b]
    std::vector<StateIndex> gate_contrib(static_cast<std::size_t>(local_dim));
    for (int p = 0; p < local_dim; ++p) {
        StateIndex contrib = 0;
        for (int b = 0; b < k; ++b) {
            if ((p >> b) & 1) {
                contrib |= StateIndex{1} << g.qubits[static_cast<std::size_t>(b)];
            }
        }
        gate_contrib[static_cast<std::size_t>(p)] = contrib;
    }

    // Precompute: the sorted gate-qubit mask (bits at gate positions are 1).
    StateIndex gate_mask = 0;
    for (int b = 0; b < k; ++b) {
        gate_mask |= StateIndex{1} << g.qubits[static_cast<std::size_t>(b)];
    }

    // Iterate over all base indices (base = full index with all gate-qubit bits = 0).
    // We enumerate them by iterating over all 2^n indices and selecting those
    // where (idx & gate_mask) == 0.
    //
    // To avoid iterating all 2^n indices, we use a "skip" enumeration:
    // base indices have certain bit positions zeroed. We enumerate them by
    // constructing them bit-by-bit, inserting 0 at each gate qubit position.
    //
    // Efficient enumeration: treat the non-gate bits as a (n-k)-bit counter,
    // then insert the gate-qubit zeros to reconstruct the base index.
    //
    // g.qubits is sorted ascending (invariant of FusedGate).
    // We build a "bit insertion" table: for each bit b_free (0..n-k-1), which
    // bit position b_full it maps to in the full index (skipping gate positions).

    // Determine n (number of qubits in state).
    // n = log2(dim).  We compute it by counting trailing zeros of dim (power of 2).
    int n = 0;
    {
        StateIndex tmp = dim;
        while (tmp > 1) { tmp >>= 1; ++n; }
    }
    const int n_free = n - k;  // number of non-gate qubits

    // Build sorted list of free bit positions (bits not in g.qubits).
    std::vector<int> free_bits;
    free_bits.reserve(static_cast<std::size_t>(n_free));
    {
        std::vector<bool> is_gate_bit(static_cast<std::size_t>(n), false);
        for (int b = 0; b < k; ++b) {
            is_gate_bit[static_cast<std::size_t>(g.qubits[static_cast<std::size_t>(b)])] = true;
        }
        for (int b = 0; b < n; ++b) {
            if (!is_gate_bit[static_cast<std::size_t>(b)]) {
                free_bits.push_back(b);
            }
        }
    }

    // Store the matrix TRANSPOSED into split re/im arrays: matT[c*ld + r] = M[r][c]
    // (g.matrix is row-major, M[r][c] = g.matrix[r*ld + c]).  The transpose makes
    // a fixed column c contiguous over r, so the column-accumulate GEMV inner loop
    // is stride-1 and vectorisable. Precomputed once per gate (cheap:
    // mat_size <= 256).
    const std::size_t mat_size = static_cast<std::size_t>(local_dim * local_dim);
    std::vector<double> matT_re(mat_size), matT_im(mat_size);
    for (int r = 0; r < local_dim; ++r) {
        for (int c = 0; c < local_dim; ++c) {
            const Cx v = g.matrix[static_cast<std::size_t>(r * local_dim + c)];
            matT_re[static_cast<std::size_t>(c * local_dim + r)] = v.real();
            matT_im[static_cast<std::size_t>(c * local_dim + r)] = v.imag();
        }
    }

    // Parallel loop over all base indices (n_base = 2^n_free independent blocks).
    // The CPUID-selected worker (AVX2 / AVX-512) runs the OpenMP parallel-for and
    // the vectorised GEMV internally (single binary).
    const std::int64_t n_base_i = static_cast<std::int64_t>(n_base);
    fused_loop_kernel()(re, im, local_dim, n_free, n_base_i,
                        free_bits.data(), gate_contrib.data(),
                        matT_re.data(), matT_im.data());
}

// make_dense_gate — operand-order matrix -> sorted-qubit FusedGate.
// Reuses embed_matrix with union == sorted(qubits): the "extra qubit" set is
// empty, so the embedding degenerates to exactly the axis permutation needed.
FusedGate make_dense_gate(const std::vector<QubitIndex>& qubits,
                          const std::vector<Cx>&         matrix) {
    const std::size_t k = qubits.size();
    if (k == 0) {
        throw SimulatorError{"FullState", "dense gate has no qubits"};
    }
    // apply_fused_gate's amp/out scratch is a fixed 1 << kMaxFusionWidth stack
    // array and does not range-check; fuse_circuit() normally guarantees the
    // bound by clamping max_width. Callers bypassing it must check here.
    if (k > static_cast<std::size_t>(kMaxFusionWidth)) {
        throw SimulatorError{"FullState",
            "dense gate spans " + std::to_string(k) +
            " qubits, exceeding the " + std::to_string(kMaxFusionWidth) +
            "-qubit kernel limit"};
    }
    const std::size_t expected = (std::size_t{1} << k) * (std::size_t{1} << k);
    if (matrix.size() != expected) {
        throw SimulatorError{"FullState",
            "dense gate matrix has " + std::to_string(matrix.size()) +
            " entries, expected " + std::to_string(expected) +
            " for " + std::to_string(k) + " qubits"};
    }

    std::vector<QubitIndex> sorted = qubits;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw SimulatorError{"FullState", "dense gate has repeated qubits"};
    }

    FusedGate g;
    g.qubits = sorted;
    // Already ascending in operand order -> permutation is the identity, so skip
    // the O(4^k) embed pass (the common 1Q case and any ascending operand list).
    g.matrix = (sorted == qubits) ? matrix
                                  : embed_matrix(matrix, qubits, sorted);
    return g;
}

}  // namespace detail
}  // namespace qplayer
