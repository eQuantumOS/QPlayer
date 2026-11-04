// dump_statevector.cpp — diagnostic tool: parse a QASM file, run it on the
// FullState CPU backend, and write the final statevector as raw little-endian
// doubles (re0, im0, re1, im1, ...) to stdout. Used to cross-validate QPlayer
// against an external reference (qiskit) when reconciling golden baselines.
// Not part of the test suite; built manually. Index convention: amplitude i
// has qubit q bit = (i>>q)&1 (matches qiskit Statevector ordering).

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "qregister.hpp"
#include "circuit/circuit.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.qasm>\n", argv[0]);
        return 2;
    }
    qplayer::QRegister qr{qplayer::SimType::FullState};
    qr.set_seed(42);
    qplayer::Circuit circ = qr.parse_file(argv[1]);
    qr.run(circ, 1);
    const qplayer::StateVector sv = qr.get_statevector();
    for (const auto& z : sv) {
        const double re = z.real();
        const double im = z.imag();
        std::fwrite(&re, sizeof(double), 1, stdout);
        std::fwrite(&im, sizeof(double), 1, stdout);
    }
    return 0;
}
