/// @file pauli_expectation.hpp
/// @brief Shared host-side Pauli expectation primitive for the FullState backends.
/// @ingroup backend
///
/// shared host-side <psi|P|psi> primitive for the
/// FullState backends.
///
/// Both FullState CPU and FullState GPU need this in two places each: the
/// one-shot expectation_value() API on the final state, and the per-shot /
/// trajectory accumulation that defines as the nshot average
/// <P> = (1/N) * sum_j <psi_j|P|psi_j> for noisy and classically-controlled
/// circuits. Keeping one implementation makes "the GPU result matches the CPU
/// reference" structural rather than a comment (it was a hand-copied duplicate
/// before per-shot wiring).
///
/// Buffers are SoA host arrays. The GPU backend downloads its device
/// state first — the noisy trajectory loop already does so for its norm check,
/// so no extra D2H is introduced.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "types.hpp"

namespace qplayer {
namespace detail {

/// A Pauli string compiled into the quantities the contraction loop needs, so
/// that parsing and validation happen once per run() rather than once per shot.
/// P maps |j> to |j ^ flip_mask> with sign (-1)^popcount(j & sign_mask), times a
/// global phase i^num_y folded into (ph_re, ph_im).
struct PauliSpec {
    StateIndex flip_mask = 0;
    StateIndex sign_mask = 0;
    double     ph_re     = 1.0;
    double     ph_im     = 0.0;
};

/// One factor of a Pauli string, as the per-qubit operator it denotes.
///
/// The mask form above is what a dense statevector contraction wants; a tensor
/// network needs the factors themselves because it applies them site by site
/// Both come from the same parse so the two
/// backends cannot disagree on what a string means.
enum class PauliOp : std::uint8_t { I = 0, X = 1, Y = 2, Z = 3 };

/// Parse + validate a Pauli string into per-qubit factors, indexed by QUBIT:
/// ops[q] acts on qubit q. Qubit order in the input is Qiskit big-endian
///: pauli[0] acts on qubit n-1. An empty string means all-Z over every
/// qubit.
///
/// Throws CircuitError on an invalid character or a length mismatch.
std::vector<PauliOp> compile_pauli_ops(std::string_view pauli, QubitIndex n_qubits);

/// Parse + validate a Pauli string against n_qubits. Qubit order is Qiskit
/// big-endian: pauli[0] acts on qubit n-1. An empty string means all-Z
/// over every qubit.
///
/// Throws CircuitError on an invalid character or a length mismatch. Callers on
/// a stochastic path must compile every target *before* entering an OpenMP
/// region — exceptions cannot cross one.
PauliSpec compile_pauli(std::string_view pauli, QubitIndex n_qubits);

/// compile_pauli() over a list of registered targets (Circuit::expectation_targets).
std::vector<PauliSpec> compile_targets(const std::vector<std::string>& targets,
                                       QubitIndex n_qubits);

/// <psi|P|psi> over an SoA host buffer. P is Hermitian, so the imaginary part is
/// numerical noise and only the real part is returned.
///
/// Stateless, so the per-shot and trajectory loops can evaluate it on their own
/// private state. Called from inside an OpenMP region (the CPU trajectory loop)
/// its internal parallel-for runs with a single thread — correct, just serial.
double pauli_expectation(const double* re, const double* im,
                         StateIndex dim, const PauliSpec& spec);

}  // namespace detail
}  // namespace qplayer
