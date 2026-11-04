/// @file tn_rdm.cpp
/// @brief Reduced density matrices from an MPS chain.
///
/// rho_Q = Tr_{Q^c} |psi><psi| is computed by sweeping the chain against its own
/// conjugate. Sites inside Q keep their physical legs open on both the ket and
/// bra side; sites outside Q have theirs contracted together, which is exactly
/// the partial trace. No statevector is built, so the cost scales with 4^|Q| and
/// chi rather than with 2^n.

#include "backend/tensor_network/mps.hpp"

#include "error.hpp"

#include <algorithm>
#include <string>

namespace qplayer {
namespace detail {

namespace {

constexpr const char* kBackend = "TensorNetwork";
constexpr std::uint32_t kLeft     = 0;
constexpr std::uint32_t kPhysical = 1;
constexpr std::uint32_t kRight    = 2;

using Cx = std::complex<double>;

}  // namespace

std::vector<Cx> mps_reduced_density_matrix(const TensorGraph& chain, std::vector<QubitIndex> sites) {
    const std::size_t n = chain.node_count();
    std::sort(sites.begin(), sites.end());
    if (std::adjacent_find(sites.begin(), sites.end()) != sites.end()) {
        throw SimulatorError{kBackend, "reduced_density_matrix received a repeated site"};
    }
    if (!sites.empty() && sites.back() >= n) {
        throw SimulatorError{kBackend, "reduced_density_matrix site out of range"};
    }
    if (sites.size() > kMaxRdmSites) {
        throw SimulatorError{kBackend,
            "reduced_density_matrix subset of " + std::to_string(sites.size()) +
            " exceeds kMaxRdmSites (" + std::to_string(kMaxRdmSites) +
            "); the matrix holds 4^k entries"};
    }

    const auto in_subset = [&sites](std::size_t site) {
        return std::binary_search(sites.begin(), sites.end(), static_cast<QubitIndex>(site));
    };

    // Environment indexed [ket open bits][bra open bits][ket bond][bra bond].
    // Open bits accumulate as the sweep passes subset sites, so the final one is
    // rho itself once both bonds close at dimension 1.
    std::size_t open = 1;   // 2^(subset sites seen so far)
    std::size_t seen = 0;   // how many subset sites have been passed
    std::size_t bond = 1;
    std::vector<Cx> env{Cx{1.0, 0.0}};

    const auto at = [](std::size_t k, std::size_t b, std::size_t ak, std::size_t ab,
                       std::size_t open_dim, std::size_t bond_dim) {
        return ((k * open_dim + b) * bond_dim + ak) * bond_dim + ab;
    };

    for (std::size_t site = 0; site < n; ++site) {
        const TensorNode& node = chain.node(static_cast<std::uint32_t>(site));
        const std::size_t left  = node.indices[kLeft].dim;
        const std::size_t right = node.indices[kRight].dim;
        if (left != bond) throw SimulatorError{kBackend, "reduced_density_matrix found an inconsistent bond"};

        const bool keep = in_subset(site);
        const std::size_t next_open = keep ? open * 2 : open;
        std::vector<Cx> next(next_open * next_open * right * right, Cx{0.0, 0.0});

        for (std::size_t k = 0; k < open; ++k) {
            for (std::size_t b = 0; b < open; ++b) {
                for (std::size_t ak = 0; ak < left; ++ak) {
                    for (std::size_t ab = 0; ab < left; ++ab) {
                        const Cx e = env[at(k, b, ak, ab, open, bond)];
                        if (e == Cx{0.0, 0.0}) continue;
                        for (std::size_t sk = 0; sk < 2; ++sk) {
                            for (std::size_t sb = 0; sb < 2; ++sb) {
                                // Outside the subset the two physical values are
                                // tied together — that tie IS the partial trace.
                                if (!keep && sk != sb) continue;
                                // New bit goes in at position `seen`, LSB first,
                                // so bit i of the result is the i-th smallest
                                // site — the order the contract states. Shifting
                                // the accumulator instead (k*2 + s) would put the
                                // FIRST site in the HIGHEST bit, silently
                                // reversing the caller's bit order.
                                const std::size_t k2 = keep ? (k | (sk << seen)) : k;
                                const std::size_t b2 = keep ? (b | (sb << seen)) : b;
                                for (std::size_t bk = 0; bk < right; ++bk) {
                                    const Cx ket = node.data[(ak * 2 + sk) * right + bk];
                                    if (ket == Cx{0.0, 0.0}) continue;
                                    for (std::size_t bb = 0; bb < right; ++bb) {
                                        const Cx bra = std::conj(node.data[(ab * 2 + sb) * right + bb]);
                                        next[at(k2, b2, bk, bb, next_open, right)] += e * ket * bra;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        env  = std::move(next);
        open = next_open;
        if (keep) ++seen;
        bond = right;
    }

    if (bond != 1) throw SimulatorError{kBackend, "reduced_density_matrix chain did not close"};

    std::vector<Cx> rho(open * open, Cx{0.0, 0.0});
    for (std::size_t k = 0; k < open; ++k) {
        for (std::size_t b = 0; b < open; ++b) rho[k * open + b] = env[at(k, b, 0, 0, open, 1)];
    }
    return rho;
}

}  // namespace detail
}  // namespace qplayer
