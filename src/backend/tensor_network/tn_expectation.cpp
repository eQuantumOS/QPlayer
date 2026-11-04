/// @file tn_expectation.cpp
/// @brief Pauli expectation values from an MPS chain.
///
/// <psi|P|psi> for a Pauli string is a partial contraction, not an export: P
/// factorises over sites, so a single left-to-right sweep carrying the (ket, bra)
/// bond pair suffices. That is what promises for
/// this backend — "supported at every qubit count, computed without a dense
/// conversion" — and it is why the cost is O(n * chi^3) rather than O(2^n).
///
/// The sweep is the squared_norm() transfer matrix with a 2x2 factor inserted at
/// each site; P = I...I reduces to exactly that, which is the cheapest available
/// check that the insertion is wired correctly.

#include "backend/tensor_network/mps.hpp"

#include "error.hpp"

#include <string>

namespace qplayer {
namespace detail {

namespace {

constexpr const char* kBackend = "TensorNetwork";
constexpr std::uint32_t kLeft     = 0;
constexpr std::uint32_t kRight    = 2;

using Cx = std::complex<double>;

// <bra|P|ket> for one factor. Row is the bra (output) index, column the ket
// (input) index, matching <psi|P|psi> = sum conj(psi_bra) P[bra][ket] psi_ket.
Cx pauli_element(PauliOp op, std::size_t bra, std::size_t ket) {
    switch (op) {
        case PauliOp::I: return bra == ket ? Cx{1.0, 0.0} : Cx{0.0, 0.0};
        case PauliOp::X: return bra != ket ? Cx{1.0, 0.0} : Cx{0.0, 0.0};
        // Y = [[0, -i], [i, 0]]: the sign follows the bra index, so getting it
        // backwards flips the sign of every Y term rather than failing loudly.
        case PauliOp::Y:
            if (bra == ket) return Cx{0.0, 0.0};
            return bra == 1 ? Cx{0.0, 1.0} : Cx{0.0, -1.0};
        case PauliOp::Z:
            if (bra != ket) return Cx{0.0, 0.0};
            return bra == 0 ? Cx{1.0, 0.0} : Cx{-1.0, 0.0};
    }
    return Cx{0.0, 0.0};
}

// Row-major (left, physical, right) offset — the site tensor layout.
std::size_t site_offset(std::size_t left_idx, std::size_t phys, std::size_t right_idx,
                        std::size_t right_dim) {
    return (left_idx * 2 + phys) * right_dim + right_idx;
}

}  // namespace

double mps_expectation_value(const TensorGraph& chain, const std::vector<PauliOp>& ops) {
    const std::size_t n = chain.node_count();
    if (ops.size() != n) {
        throw SimulatorError{kBackend,
            "expectation_value received " + std::to_string(ops.size()) +
            " Pauli factors for a chain of " + std::to_string(n) + " sites"};
    }

    // env[ak * bond + ab] — ak indexes the ket bond, ab the bra bond. Both open
    // bonds start at dimension 1 (the chain's left boundary).
    std::vector<Cx> env{Cx{1.0, 0.0}};
    std::size_t bond = 1;

    for (std::size_t site = 0; site < n; ++site) {
        const TensorNode& node = chain.node(static_cast<std::uint32_t>(site));
        const std::size_t left  = node.indices[kLeft].dim;
        const std::size_t right = node.indices[kRight].dim;
        if (left != bond) {
            throw SimulatorError{kBackend, "expectation_value found an inconsistent bond"};
        }

        std::vector<Cx> next(right * right, Cx{0.0, 0.0});
        for (std::size_t ak = 0; ak < left; ++ak) {
            for (std::size_t ab = 0; ab < left; ++ab) {
                const Cx e = env[ak * bond + ab];
                if (e == Cx{0.0, 0.0}) continue;
                for (std::size_t sk = 0; sk < 2; ++sk) {
                    for (std::size_t sb = 0; sb < 2; ++sb) {
                        const Cx p = pauli_element(ops[site], sb, sk);
                        if (p == Cx{0.0, 0.0}) continue;
                        const Cx ep = e * p;
                        for (std::size_t bk = 0; bk < right; ++bk) {
                            const Cx ket = node.data[site_offset(ak, sk, bk, right)];
                            if (ket == Cx{0.0, 0.0}) continue;
                            for (std::size_t bb = 0; bb < right; ++bb) {
                                const Cx bra =
                                    std::conj(node.data[site_offset(ab, sb, bb, right)]);
                                next[bk * right + bb] += ep * ket * bra;
                            }
                        }
                    }
                }
            }
        }

        env  = std::move(next);
        bond = right;
    }

    if (bond != 1 || env.empty()) {
        throw SimulatorError{kBackend, "expectation_value chain did not close"};
    }
    return env[0].real();
}

}  // namespace detail
}  // namespace qplayer
