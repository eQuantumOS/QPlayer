/// @file tn_amplitude.cpp
/// @brief Single-amplitude evaluation on an MPS chain.
///
/// Fixing every physical leg to a known value turns the chain into a product of
/// matrices, so the amplitude falls out of one left-to-right sweep at O(n*chi^2).
/// Nothing of size 2^n is ever allocated — which is what makes this mode useful
/// on circuits far past the statevector limit.

#include "backend/tensor_network/mps.hpp"

#include "error.hpp"

#include <string>

namespace qplayer {
namespace detail {

namespace {

constexpr const char* kBackend = "TensorNetwork";
constexpr std::uint32_t kLeft     = 0;
constexpr std::uint32_t kPhysical = 1;
constexpr std::uint32_t kRight    = 2;

}  // namespace

std::complex<double> mps_amplitude(const TensorGraph& chain, const std::vector<int>& bits) {
    if (bits.size() != chain.node_count()) {
        throw SimulatorError{kBackend,
            "amplitude bitstring length " + std::to_string(bits.size()) +
            " does not match the chain length " + std::to_string(chain.node_count())};
    }

    // Carries the partial row vector over the current open bond.
    std::vector<std::complex<double>> vec{std::complex<double>{1.0, 0.0}};
    std::size_t bond = 1;

    for (std::size_t site = 0; site < chain.node_count(); ++site) {
        const TensorNode& node = chain.node(static_cast<std::uint32_t>(site));
        const std::size_t left  = node.indices[kLeft].dim;
        const std::size_t right = node.indices[kRight].dim;
        if (left != bond) throw SimulatorError{kBackend, "amplitude found an inconsistent bond dimension"};

        const int b = bits[site];
        if (b != 0 && b != 1) {
            throw SimulatorError{kBackend, "amplitude bitstring entries must be 0 or 1"};
        }
        const std::size_t s = static_cast<std::size_t>(b);

        std::vector<std::complex<double>> next(right, std::complex<double>{0.0, 0.0});
        for (std::size_t r = 0; r < right; ++r) {
            std::complex<double> acc{0.0, 0.0};
            for (std::size_t a = 0; a < left; ++a) {
                acc += vec[a] * node.data[(a * 2 + s) * right + r];
            }
            next[r] = acc;
        }
        vec = std::move(next);
        bond = right;
    }

    // The right boundary bond is dimension 1, so the sweep ends on a scalar.
    return vec.empty() ? std::complex<double>{0.0, 0.0} : vec[0];
}

}  // namespace detail
}  // namespace qplayer
