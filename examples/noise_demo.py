#!/usr/bin/env python3
"""noise_demo.py — Per-channel NoiseModel + DeviceModel demonstration.

One test case per noise channel (..), a combined multi-channel
case, and a device-noise case parsed from a JSON *string* (not a file). Each
case runs the same circuit twice — once ideal (no noise) and once noisy — and
prints the measured probability distributions side by side.

Why distributions and not raw statevectors: a noisy run is a Monte Carlo
ensemble over trajectories (a mixed state), so a single post-run statevector is
not a meaningful "after" state. The measured Z-basis probability distribution is
the well-defined observable, so before/after is compared on counts.

Run:  python noise_demo.py        (after `pip install qplayer`,
      or with PYTHONPATH=bindings)
"""

import math

import qplayer

SHOTS = 8192
SEED  = 42


def measure_distribution(noise_model, build, num_qubits, num_cubits,
                         shots=SHOTS, seed=SEED):
    """Run `build(circ)` on a fresh CPU FullState register, return P(state)."""
    qr = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
    qr.set_seed(seed)
    if noise_model is not None:
        qr.set_noise_model(noise_model)

    circ = qplayer.Circuit(num_qubits, num_cubits)
    build(circ)

    counts = qr.run(circ, shots).counts()
    total  = sum(counts.values()) or 1
    return {state: cnt / total for state, cnt in counts.items()}


def report(title, detail, ideal, noisy):
    """Print a side-by-side ideal-vs-noisy distribution table."""
    keys = sorted(set(ideal) | set(noisy))
    print(title)
    print(f"  {detail}")
    print(f"    {'state':>6} | {'ideal':>7} | {'noisy':>7} | {'delta':>8}")
    print(f"    {'-' * 6}-+-{'-' * 7}-+-{'-' * 7}-+-{'-' * 8}")
    for k in keys:
        p_i = ideal.get(k, 0.0)
        p_n = noisy.get(k, 0.0)
        print(f"    |{k}> | {p_i:7.3f} | {p_n:7.3f} | {p_n - p_i:+8.3f}")
    print()


def run_case(title, detail, noise_model, build, num_qubits, num_cubits):
    """Run one channel's circuit ideal vs noisy and report the comparison."""
    ideal = measure_distribution(None,        build, num_qubits, num_cubits)
    noisy = measure_distribution(noise_model, build, num_qubits, num_cubits)
    report(title, detail, ideal, noisy)


# ── Single-channel cases (..) ────────────────────────────────

def case_depolarizing():
    """depolarizing channel on X. |1> leaks toward |0>."""
    nm = qplayer.NoiseModel().add_depolarizing("x", 0.10)
    run_case(
        "Case 1 — Depolarizing channel on X gate",
        "p=0.10; ideal |1>=1.000, depolarizing mixes toward maximally mixed",
        nm, lambda c: c.x(0).measure(0, 0), 1, 1)


def case_amplitude_damping():
    """amplitude damping (T1 decay) on X. |1> relaxes to |0>."""
    nm = qplayer.NoiseModel().add_amplitude_damping("x", 0.20)
    run_case(
        "Case 2 — Amplitude damping on X gate",
        "gamma=0.20; ideal |1>=1.000, T1 decay pulls |1>->|0> (~0.20 to |0>)",
        nm, lambda c: c.x(0).measure(0, 0), 1, 1)


def case_phase_damping():
    """phase damping (T2 dephasing). Invisible in Z basis, so use an
    H-sandwich: H|0>=|+>, dephase, H back. Ideal returns |0>; dephasing drifts
    the outcome toward 50/50. The channel fires on each H gate."""
    nm = qplayer.NoiseModel().add_phase_damping("h", 0.50)
    run_case(
        "Case 3 — Phase damping on H gate",
        "lam=0.50 per H; ideal H.H|0>=|0>=1.000, dephasing erases coherence -> ~50/50",
        nm, lambda c: c.h(0).h(0).measure(0, 0), 1, 1)


def case_bit_flip():
    """bit flip (X with prob p) on X. Flips |1> back to |0>."""
    nm = qplayer.NoiseModel().add_bit_flip("x", 0.15)
    run_case(
        "Case 4 — Bit flip on X gate",
        "p=0.15; ideal |1>=1.000, bit flip returns to |0> with prob ~0.15",
        nm, lambda c: c.x(0).measure(0, 0), 1, 1)


def case_phase_flip():
    """phase flip (Z with prob p). Z-invisible, so H-sandwich again.
    A Z between two H gates becomes an X, so an odd number of flips toggles the
    outcome. The channel fires on each H gate."""
    nm = qplayer.NoiseModel().add_phase_flip("h", 0.20)
    run_case(
        "Case 5 — Phase flip on H gate",
        "p=0.20 per H; ideal H.H|0>=|0>=1.000, phase flips (Z) surface as |1>",
        nm, lambda c: c.h(0).h(0).measure(0, 0), 1, 1)


def case_readout_error():
    """readout error. Classical measurement bit-flip, no state change.
    p_meas_0_given_1 = P(read 0 | true 1)."""
    nm = qplayer.NoiseModel().add_readout_error(0, 0.10, 0.0)
    run_case(
        "Case 6 — Readout error",
        "P(0|1)=0.10, P(1|0)=0.00; ideal |1>=1.000, misreads as |0> ~0.10",
        nm, lambda c: c.x(0).measure(0, 0), 1, 1)


def case_raw_kraus():
    """raw user-supplied Kraus operators. Here an explicit amplitude
    damping channel: K0=diag(1, sqrt(1-g)), K1=[[0,sqrt(g)],[0,0]], which
    satisfies K0^dag K0 + K1^dag K1 = I."""
    g = 0.30
    ch = qplayer.KrausChannel()
    ch.operators = [
        [[1.0, 0.0], [0.0, math.sqrt(1.0 - g)]],
        [[0.0, math.sqrt(g)], [0.0, 0.0]],
    ]
    ch.target_qubit = 0
    ch.type = qplayer.ChannelType.Custom
    nm = qplayer.NoiseModel().add_kraus("x", ch)
    run_case(
        "Case 7 — Raw Kraus channel on X gate",
        "custom amplitude-damping Kraus, g=0.30; ideal |1>=1.000, decays ~0.30 to |0>",
        nm, lambda c: c.x(0).measure(0, 0), 1, 1)


# ── Multi-channel injection ────────────────────────────────────────────────

def case_multi_channel():
    """Several channels on one Bell circuit: depolarizing on H and CX, amplitude
    damping on CX, and readout error on both qubits. Ideal Bell is 50/50 on
    |00>/|11>; combined noise leaks population into |01>/|10>."""
    nm = (qplayer.NoiseModel()
          .add_depolarizing("h", 0.02)
          .add_depolarizing("cx", 0.04)
          .add_amplitude_damping("cx", 0.02)
          .add_readout_error(0, 0.03, 0.03)
          .add_readout_error(1, 0.03, 0.03))
    run_case(
        "Case 8 — Multiple channels on a Bell circuit",
        "depol(h)=0.02, depol(cx)=0.04, amp_damp(cx)=0.02, readout(q0,q1)=0.03; "
        "ideal |00>=|11>=0.500",
        nm, lambda c: c.h(0).cx(0, 1).measure(0, 0).measure(1, 1), 2, 2)


# ── Device noise from a JSON string (not a file) ───────────────────────────

def case_device_noise_from_string():
    """parse a DeviceModel from a JSON *string*, synthesise
    T1/T2 relaxation channels with load_device(), and apply them via the gate's
    duration. The X gate's 70 ns duration against T1=100 ns produces a visible
    amplitude-damping decay (gamma = 1 - exp(-70/100) ~= 0.50)."""
    device_json = """{
  "qubits": [
    {"T1": 100.0, "T2": 80.0}
  ],
  "gates": {
    "x": {"duration_ns": 70.0, "error_rate": 0.0}
  },
  "coupling_map": []
}"""
    dm = qplayer.DeviceModel.from_json_string(device_json)
    nm = qplayer.NoiseModel()
    nm.load_device(dm)

    print("Case 9 — Device noise from JSON string")
    print(f"  parsed: num_qubits={dm.num_qubits()}, "
          f"T1(q0)={dm.T1(0):.1f} ns, T2(q0)={dm.T2(0):.1f} ns, "
          f"has_t1_t2={nm.has_t1_t2()}")

    ideal = measure_distribution(None, lambda c: c.x(0).measure(0, 0), 1, 1)
    noisy = measure_distribution(nm,   lambda c: c.x(0).measure(0, 0), 1, 1)
    report("",
           "X duration 70 ns vs T1 100 ns; ideal |1>=1.000, "
           "relaxation decays ~0.50 to |0>",
           ideal, noisy)


def main():
    case_depolarizing()
    case_amplitude_damping()
    case_phase_damping()
    case_bit_flip()
    case_phase_flip()
    case_readout_error()
    case_raw_kraus()
    case_multi_channel()
    case_device_noise_from_string()
    print("All noise demo cases completed successfully.")


if __name__ == "__main__":
    main()
