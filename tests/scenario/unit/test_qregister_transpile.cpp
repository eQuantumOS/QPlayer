// test_qregister_transpile.cpp — Unit tests for QRegister::transpile() facade
//
//
// QRegister::transpile() delegates to Transpiler::transpile().
// Tests verify the delegation contract, Level 2/3 rejection, and
// that device_model_ / noise_model_ pointers are forwarded (nullptr when unset).
//
// Note: set_noise_model() and load_device_model() are used only to confirm that
// transpile() does not throw when those members are set/unset. The actual
// noise/device path is a Phase 6+ stub (device_json is rejected conservatively).

#include "../../framework/assertions.hpp"
#include "../../framework/test_case.hpp"

#include <cstddef>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::NoiseModel;
using qplayer::NotImplementedError;
using qplayer::QRegister;
using qplayer::QubitIndex;

// Helper: clean 2-qubit circuit (no opaque gates).
static Circuit make_bell_like() {
    Circuit c(2, 2);
    c.h(0).cx(0, 1);
    return c;
}

// ── Delegation basic ─────────────────────────────────────────────────────────

TEST(QRegisterTranspile, Level1_Default_NoThrow) {
    // QRegister::transpile() must delegate to Transpiler without throwing for a
    // clean Level-1 circuit.
    QRegister qr;
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 1));
}

TEST(QRegisterTranspile, Level0_NoThrow) {
    // Level 0 (validation only) should succeed for a clean circuit.
    QRegister qr;
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 0));
}

TEST(QRegisterTranspile, Level1_HHPair_CancelledToZeroOps) {
    // Regression: H H on same qubit must cancel after Level-1 transpile.
    Circuit c(1);
    c.h(0);
    c.h(0);

    QRegister qr;
    Circuit out = qr.transpile(std::move(c), 1);
    EXPECT_EQ(out.num_ops(), std::size_t{0});
}

// ── Level 2 / 3 activation ───────────────────────────────────────

TEST(QRegisterTranspile, Level2_RunsWithoutThrow) {
    // Level 2 activates 1Q safe reduction passes (resolved).
    QRegister qr;
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 2));
}

TEST(QRegisterTranspile, Level3_RunsWithoutThrow) {
    // Level 3 adds 2Q merge + qubit pruning passes.
    QRegister qr;
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 3));
}

TEST(QRegisterTranspile, OutOfRangeLevel_Throws_CircuitError) {
    // Level <0 or >3 stays as CircuitError.
    QRegister qr;
    EXPECT_THROW(qr.transpile(make_bell_like(), 4), CircuitError);
    EXPECT_THROW(qr.transpile(make_bell_like(), -1), CircuitError);
}

// ── device_json handling ─────────────────────────────────────────────────────

TEST(QRegisterTranspile, EmptyDeviceJson_NoThrow) {
    // Empty device_json (default) is the normal device-less path.
    QRegister qr;
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 1, ""));
}

TEST(QRegisterTranspile, NonEmptyDeviceJson_RoutesWithoutThrow) {
    // Valid device_json now drives the coupling-map pipeline. An empty
    // coupling_map means full connectivity, so routing is a no-op and the
    // compile completes (② ③).
    QRegister qr;
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 1, "{\"coupling_map\":[]}"));
}

// ── noise_model member forwarded correctly ───────────────────────────────────

TEST(QRegisterTranspile, WithNoiseModelSet_Level1_NoThrow) {
    // set_noise_model() stores the model; transpile() forwards nullptr/ptr
    // correctly. Since device_json is empty and level=1, it should succeed
    // (noise_model_ pointer is forwarded but not used in Phase 4 transpile).
    QRegister qr;
    NoiseModel nm;
    qr.set_noise_model(nm);
    // Transpile succeeds — noise_model_ pointer is forwarded but not acted upon
    // in Phase 4 (noise pipeline is Phase 8).
    EXPECT_NO_THROW(qr.transpile(make_bell_like(), 1));
}
