#!/usr/bin/env python3
"""comparison.py -- device-noise validation harness for QPlayer.

=============================================================================
PLAIN-LANGUAGE OVERVIEW (read this first)
=============================================================================
Goal: check that QPlayer's built-in noise reproduces a real IBM quantum
computer as closely as Qiskit's own hardware model does.

For every (device, circuit) pair we run the SAME circuit through THREE engines
and compare the measurement results:

  ENGINE 1  Ideal    -- QPlayer with NO noise. The perfect textbook answer.
  ENGINE 2  IBMQ     -- Qiskit Aer's faithful IBM-hardware noise model
                        (AerSimulator.from_backend). This is our reference:
                        "what the real machine would do".
  ENGINE 3  QPlayer  -- QPlayer's OWN noise, built entirely by
                        NoiseModel.load_device from the committed device JSON.

The headline question is: does ENGINE 3 (QPlayer) match ENGINE 2 (IBMQ)? The
closer they agree, the more faithfully QPlayer reproduces the device noise.

We report TWO kinds of evidence ("tiers"):

  TIER 1  Coherence numbers -- the T1/T2 relaxation times IBM reports vs. the
          ones QPlayer loads back. If they match, the device JSON round-trips.
  TIER 2  Measured distributions -- the probability of each measurement
          outcome from each engine, plus agreement scores (Hellinger fidelity,
          total-variation distance, and a plain "% agreement").

=============================================================================
TECHNICAL DETAIL
=============================================================================
``load_device`` synthesises the FULL device noise on its own -- per-gate
relaxation (amplitude damping + dephasing from T1/T2 + gate duration),
excess-over-relaxation depolarizing for 1-qubit AND 2-qubit native gates (the
Qiskit ``from_backend`` decomposition that avoids double-counting the relaxation
already inside the reported gate error), and per-qubit readout error. So this
harness does NOT layer manual depolarizing/readout channels on top.

Both the IBMQ and the QPlayer runs execute the SAME circuit: the logical QASM is
transpiled once with qiskit against the fake backend (basis gates + coupling
map), and that single transpiled circuit is fed to Aer and translated
gate-for-gate into a QPlayer Circuit. This isolates the noise model from
transpilation differences.

With ``--plot-dir DIR`` and matplotlib available, two kinds of figure are written
and their paths printed for embedding:
  - ``DIR/<device>_T1T2.png``    : qubit-0 T1/T2 relaxation sweep MEASURED on each
                                   engine's own channel (QPlayer load_device vs
                                   Aer thermal_relaxation) over increasing idle
                                   time, each fit to exp(-t/T) -> smooth curve +
                                   fitted time constant. Two panels (amplitude
                                   damping P(|1>), dephasing <X>).
  - ``DIR/<device>_<circuit>.png``: per-combination distribution comparison
                                   (grouped state-probability bars + a
                                   Hellinger-fidelity panel), only when the
                                   distribution tier ran.

Full-statevector noisy simulation is per-shot Monte Carlo, so its cost scales as
shots x 2^n, where n is the number of qubits QPlayer actually simulates. A small
circuit mapped onto a wide device (e.g. FakeGeneva's 27 qubits) touches only a few
physical qubits, so when the device is wider than ``--max-sim-qubits`` (default 14)
QPlayer's own device-bound QubitTruncationPass compresses the circuit
onto its active physical qubits (dense [0,k)); QPlayer then simulates 2^k and
``run()`` projects each qubit's physical device noise onto that range
(project_to_dense). Aer prunes idle qubits internally, so it keeps running the
full transpiled circuit as the reference. Only if even the active-qubit circuit
exceeds the limit is the distribution tier skipped (reported explicitly).

Every backend (qiskit, qiskit-aer, qiskit-ibm-runtime, qplayer, matplotlib) is an
OPTIONAL dependency: a missing import is reported as a SKIP / NOTE, never a hard
failure.

Usage:
    # single combination -> stdout (optionally plots via --plot-dir)
    python comparison.py <device> <circuit.qasm> [--shots N] [--seed S]
                                                 [--max-sim-qubits M]
                                                 [--plot-dir DIR]
        <device> = manila | oslo | geneva  (or a path to a device JSON file)

    # sweep the full device x circuit matrix -> markdown report + PNG plots
    python comparison.py --report [--shots N] [--ansatz-shots N] [--seed S]

The T1/T2 decay curves are MEASURED (Monte-Carlo) on each engine's own relaxation
channel over an idle-time sweep and then fit to exp(-t/T): the fit gives a smooth
curve plus a per-engine fitted time constant, so QPlayer-vs-IBMQ shows a real
(statistical + engine) difference rather than a re-plot of identical parameters.
"""

import argparse
import glob
import io
import json
import math
import os
import platform
import sys
from contextlib import redirect_stdout
from datetime import datetime

# Silence QPlayer's C++ diagnostic logs (e.g. "[QPlayer/WARN ] ...") so they do
# not clutter the console or the captured report. QPlayer reads QPLAYER_LOG_LEVEL
# once at process start; "error" keeps genuine errors but drops WARN/INFO/DEBUG.
# setdefault leaves an explicit user override (e.g. QPLAYER_LOG_LEVEL=debug) intact.
os.environ.setdefault("QPLAYER_LOG_LEVEL", "error")

# Folders next to this script.
HERE = os.path.dirname(os.path.abspath(__file__))
DEVICE_JSON_DIR = os.path.join(HERE, "device_json")   # committed device calibrations
QASM_DIR = os.path.join(HERE, "qasm")                 # the circuits we test
RESULT_DIR = os.path.join(HERE, "result")             # where --report writes output

# The IBM devices we validate against.
#   short name -> (Qiskit fake-backend class, committed device JSON file)
# All three are IBM Falcon processors (native basis {id, rz, sx, x, cx}).
# FakeManilaV2 keeps the V2 suffix; Oslo/Geneva were added as BackendV2 directly.
DEVICES = {
    "manila": ("FakeManilaV2", "fake_manila_v2.json"),
    "oslo":   ("FakeOslo",     "fake_oslo.json"),
    "geneva": ("FakeGeneva",   "fake_geneva.json"),
}

# Distribution-tier shot floor per active state. A spread distribution over
# 2^k states needs shots >> 2^k to estimate meaningfully: with ~1 shot/state the
# Hellinger fidelity is dominated by sampling noise (two independent samples of
# the SAME distribution already disagree), not by model mismatch. Empirically
# ~16 shots/state lifts a 12q ansatz off that floor (Fake-vs-QPlayer 0.55 @ 1x
# -> 0.96 @ 16x). The distribution tier therefore uses at least
# SHOTS_PER_STATE * 2^(active width); the requested --shots / --ansatz-shots is a
# FLOOR, so narrow circuits (manila 5q, oslo 7q, all generic <=5q) are unchanged
# and only wide active widths (e.g. geneva's 12q ansatz) auto-scale up.
SHOTS_PER_STATE = 16


# =============================================================================
# SECTION 1 -- Comparing two probability distributions
# -----------------------------------------------------------------------------
# A "distribution" here is a dict {measurement outcome (bit string): probability}.
# These small functions turn raw shot counts into probabilities and score how
# similar two distributions are.
# =============================================================================

def counts_to_probabilities(counts: dict) -> dict:
    """Turn raw shot counts {"01": 480, ...} into probabilities that sum to 1."""
    total = sum(counts.values()) or 1
    return {outcome: n / total for outcome, n in counts.items()}


def reverse_bit_order(probs: dict) -> dict:
    """Flip every bit-string key end-to-end (e.g. "011" -> "110")."""
    return {outcome[::-1]: p for outcome, p in probs.items()}


def total_variation_distance(p: dict, q: dict) -> float:
    """How far apart two distributions are, in [0, 1]. 0 = identical.

    It is half the summed absolute difference of every outcome's probability.
    Equivalently, ``1 - total_variation_distance`` is the fraction of
    probability mass the two distributions literally share (their "% overlap").
    """
    outcomes = set(p) | set(q)
    return 0.5 * sum(abs(p.get(o, 0.0) - q.get(o, 0.0)) for o in outcomes)


def hellinger_fidelity(p: dict, q: dict) -> float:
    """Statistical similarity of two distributions, in [0, 1]. 1 = identical.

    This is the classical fidelity (the squared Bhattacharyya overlap) and is
    the report's headline "how well do they agree" score.
    """
    outcomes = set(p) | set(q)
    overlap = sum(math.sqrt(p.get(o, 0.0) * q.get(o, 0.0)) for o in outcomes)
    return overlap * overlap


def match_bit_order(reference: dict, other: dict):
    """Return ``other`` in whichever bit order best matches ``reference``.

    Qiskit and QPlayer can label the same outcome with opposite bit order
    (endianness) or different multi-register spacing. We pick identity vs
    fully-reversed, whichever is closer to the reference, so the comparison is
    fair. Returns (aligned_distribution, "identity" | "reversed").
    """
    distance_as_is = total_variation_distance(reference, other)
    reversed_other = reverse_bit_order(other)
    distance_reversed = total_variation_distance(reference, reversed_other)
    if distance_reversed < distance_as_is - 1e-12:
        return reversed_other, "reversed"
    return other, "identity"


# =============================================================================
# SECTION 2 -- Translating a Qiskit circuit into a QPlayer circuit
# -----------------------------------------------------------------------------
# After transpilation the circuit is a list of gates. We rebuild the exact same
# gate list on a QPlayer circuit so both engines run byte-for-byte the same
# operations. The table below says how to apply each Qiskit gate on QPlayer.
# =============================================================================

def qiskit_gate_to_qplayer_table():
    """Map a Qiskit gate name to the QPlayer call that applies it.

    Each entry is ``name -> function(circuit, qubits, params)``. Covers the IBM
    basis set {id, rz, sx, x, cx} plus common extras so a lightly-transpiled
    circuit still translates.
    """
    return {
        "id":   lambda c, q, p: c.id(q[0]),
        "x":    lambda c, q, p: c.x(q[0]),
        "y":    lambda c, q, p: c.y(q[0]),
        "z":    lambda c, q, p: c.z(q[0]),
        "h":    lambda c, q, p: c.h(q[0]),
        "sx":   lambda c, q, p: c.sx(q[0]),
        "sxdg": lambda c, q, p: c.sxdg(q[0]),
        "s":    lambda c, q, p: c.s(q[0]),
        "sdg":  lambda c, q, p: c.sdg(q[0]),
        "t":    lambda c, q, p: c.t(q[0]),
        "tdg":  lambda c, q, p: c.tdg(q[0]),
        "rx":   lambda c, q, p: c.rx(float(p[0]), q[0]),
        "ry":   lambda c, q, p: c.ry(float(p[0]), q[0]),
        "rz":   lambda c, q, p: c.rz(float(p[0]), q[0]),
        "u1":   lambda c, q, p: c.u1(float(p[0]), q[0]),
        "u2":   lambda c, q, p: c.u2(float(p[0]), float(p[1]), q[0]),
        "u3":   lambda c, q, p: c.u3(float(p[0]), float(p[1]), float(p[2]), q[0]),
        "cx":   lambda c, q, p: c.cx(q[0], q[1]),
        "cz":   lambda c, q, p: c.cz(q[0], q[1]),
        "cy":   lambda c, q, p: c.cy(q[0], q[1]),
        "swap": lambda c, q, p: c.swap(q[0], q[1]),
    }


def translate_circuit(qiskit_circuit, qplayer):
    """Rebuild a transpiled Qiskit circuit as a qplayer.Circuit, gate for gate.

    Qubit and clbit indices are preserved. Barriers are dropped (they are a
    simulation no-op). Raises ValueError on an untranslatable gate.
    """
    gate_table = qiskit_gate_to_qplayer_table()
    qubit_index = {bit: i for i, bit in enumerate(qiskit_circuit.qubits)}
    clbit_index = {bit: i for i, bit in enumerate(qiskit_circuit.clbits)}
    circuit = qplayer.Circuit(qiskit_circuit.num_qubits, qiskit_circuit.num_clbits)

    for instruction in qiskit_circuit.data:
        operation = instruction.operation
        name = operation.name
        qubits = [qubit_index[b] for b in instruction.qubits]
        clbits = [clbit_index[b] for b in instruction.clbits]
        if name == "barrier":
            continue
        if name == "measure":
            circuit.measure(qubits[0], clbits[0])
            continue
        if name == "reset":
            circuit.reset(qubits[0])
            continue
        apply_gate = gate_table.get(name)
        if apply_gate is None:
            raise ValueError(f"unsupported gate '{name}' after transpile")
        apply_gate(circuit, qubits, operation.params)
    return circuit


# =============================================================================
# SECTION 3 -- Building QPlayer's noise model from the device calibration
# =============================================================================

def build_qplayer_noise_model(calibration: dict, qplayer):
    """Build the QPlayer NoiseModel purely from the device JSON.

    ``load_device`` synthesises relaxation + excess-over-relaxation depolarizing
    (1Q and 2Q) + readout error on its own from the device fields, so this is
    the whole assembly -- no manual channels are layered on. Returns
    (noise_model, device_model).
    """
    device_model = qplayer.DeviceModel.from_json_string(json.dumps(calibration))
    noise_model = qplayer.NoiseModel()
    noise_model.load_device(device_model)
    return noise_model, device_model


# =============================================================================
# SECTION 4 -- Small formatting / physics helpers
# =============================================================================

def count_gates(qiskit_circuit):
    """Count how many of each gate a circuit contains -> {gate name: count}."""
    histogram = {}
    for instruction in qiskit_circuit.data:
        name = instruction.operation.name
        histogram[name] = histogram.get(name, 0) + 1
    return histogram


def format_gate_counts(histogram):
    """Render a gate histogram as "cxx8, hx5, ..." (most frequent first)."""
    return ", ".join(f"{name}x{count}" for name, count in
                     sorted(histogram.items(), key=lambda kv: (-kv[1], kv[0])))


def dephasing_time(t1, t2):
    """Pure-dephasing time T_phi from 1/T2 = 1/(2 T1) + 1/T_phi (nanoseconds)."""
    if not (t1 and t2):
        return None
    inverse_t_phi = 1.0 / t2 - 1.0 / (2.0 * t1)
    return (1.0 / inverse_t_phi) if inverse_t_phi > 0 else float("inf")


# =============================================================================
# SECTION 5 -- The three engines (each returns a probability distribution)
# -----------------------------------------------------------------------------
# These three tiny functions are the heart of the comparison. Each runs the same
# circuit and returns {outcome: probability}. Keeping them separate makes the
# "Ideal vs IBMQ vs QPlayer" story obvious.
# =============================================================================

def run_ideal_engine(qplayer, qplayer_circuit, shots, seed):
    """ENGINE 1/3 -- Ideal: QPlayer with NO noise (the perfect textbook result)."""
    simulator = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
    simulator.set_seed(seed)
    return counts_to_probabilities(simulator.run(qplayer_circuit, shots).counts())


def run_qplayer_engine(qplayer, qplayer_circuit, calibration, shots, seed):
    """ENGINE 2/3 -- QPlayer: QPlayer WITH its own noise (load_device only)."""
    noise_model, _ = build_qplayer_noise_model(calibration, qplayer)
    simulator = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
    simulator.set_seed(seed)
    simulator.set_noise_model(noise_model)
    return counts_to_probabilities(simulator.run(qplayer_circuit, shots).counts())


def run_ibmq_engine(aer_simulator, transpiled_circuit, shots, seed):
    """ENGINE 3/3 -- IBMQ: Qiskit Aer's faithful hardware noise model.

    Returns None when the fake backend / Aer is unavailable (then the report
    simply omits the IBMQ column).
    """
    if aer_simulator is None:
        return None
    result = aer_simulator.run(transpiled_circuit, shots=shots,
                               seed_simulator=seed).result()
    return counts_to_probabilities(dict(result.get_counts()))


# =============================================================================
# SECTION 6 -- Plots (optional; only if matplotlib is installed)
# =============================================================================

def plot_distribution_comparison(path, title, ideal, ibmq, qplayer_noisy, fidelities):
    """Grouped state-probability bars (Ideal / IBMQ / QPlayer) plus a
    Hellinger-fidelity panel. Returns the written path, or None if matplotlib is
    unavailable."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return None

    all_outcomes = set(ideal) | (set(ibmq) if ibmq else set()) | set(qplayer_noisy)
    by_probability = sorted(all_outcomes, key=lambda s: -(ideal.get(s, 0)
                            + (ibmq.get(s, 0) if ibmq else 0) + qplayer_noisy.get(s, 0)))
    top_outcomes = by_probability[:8]
    x = range(len(top_outcomes))

    series = [("Ideal", ideal, "#8899aa"),
              ("IBMQ", ibmq, "#d1495b"),
              ("QPlayer", qplayer_noisy, "#2a7fbf")]
    series = [(label, dist, color) for (label, dist, color) in series if dist is not None]
    num_series = len(series)
    bar_width = 0.8 / max(1, num_series)

    fig, (ax_bars, ax_fidelity) = plt.subplots(1, 2, figsize=(11, 4.2),
                                               gridspec_kw={"width_ratios": [3, 1]})
    for i, (label, dist, color) in enumerate(series):
        offsets = [xi + (i - (num_series - 1) / 2) * bar_width for xi in x]
        ax_bars.bar(offsets, [dist.get(o, 0.0) for o in top_outcomes],
                    bar_width, label=label, color=color)
    ax_bars.set_xticks(list(x))
    ax_bars.set_xticklabels(top_outcomes, rotation=45, ha="right",
                            fontsize=8, family="monospace")
    ax_bars.set_ylabel("probability")
    ax_bars.set_title(title)
    ax_bars.legend(fontsize=8)
    ax_bars.grid(axis="y", alpha=0.3)

    labels, values, colors = [], [], []
    if fidelities.get("ideal_ibmq") is not None:
        labels.append("Ideal-IBMQ"); values.append(fidelities["ideal_ibmq"]); colors.append("#d1495b")
    labels.append("Ideal-QP"); values.append(fidelities["ideal_qp"]); colors.append("#2a7fbf")
    if fidelities.get("ibmq_qp") is not None:
        labels.append("IBMQ-QP\n(key)"); values.append(fidelities["ibmq_qp"]); colors.append("#2a9d5a")
    ax_fidelity.bar(range(len(values)), values, color=colors)
    ax_fidelity.set_xticks(range(len(labels)))
    ax_fidelity.set_xticklabels(labels, fontsize=8)
    ax_fidelity.set_ylim(0.0, 1.05)
    ax_fidelity.set_ylabel("Hellinger fidelity")
    ax_fidelity.set_title("distribution agreement")
    ax_fidelity.grid(axis="y", alpha=0.3)
    for i, v in enumerate(values):
        ax_fidelity.text(i, min(v + 0.02, 1.0), f"{v:.3f}", ha="center", fontsize=7)

    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return path


# -- measured T1/T2 relaxation sweep (both engines' OWN channel) -------------
# For each idle time t the "wait" is one id gate of duration t, and each engine
# runs its authentic relaxation channel: QPlayer's load_device (amplitude damping
# + dephasing) and Aer's thermal_relaxation_error(T1,T2,t). The measured points
# are then fit to exp(-t/T) to recover a smooth curve + a fitted time constant per
# engine, so the plot is smooth AND shows the real (statistical + engine)
# difference between the two fits. Readout is stripped so the curve is pure
# relaxation. Qubit 0 only (1-qubit, cheap for every device width).

def relaxation_time_grid(tau_ns, points=16, span=2.5):
    """A list of idle times from 0 to span*tau, evenly spaced (nanoseconds)."""
    if not tau_ns or tau_ns <= 0.0:
        return []
    t_max = span * float(tau_ns)
    return [t_max * i / (points - 1) for i in range(points)]


def relaxation_observable(counts, kind):
    """Reduce shot counts to the quantity that decays as exp(-t/T).

    kind="t1": P(|1>) (amplitude damping); kind="t2": <X> (dephasing).
    """
    total = sum(counts.values()) or 1
    if kind == "t1":
        return counts.get("1", 0) / total            # P(|1>) -> exp(-t/T1)
    return 2.0 * (counts.get("0", 0) / total) - 1.0  # <X> -> exp(-t/T2)


def measure_qplayer_relaxation(qplayer, calibration, delays_ns, kind, shots, seed):
    """QPlayer's authentic relaxation channel (load_device), readout stripped."""
    qubits = [{k: v for k, v in qubit.items() if k in ("T1", "T2")}
              for qubit in calibration.get("qubits", [])]
    values = []
    for i, t_ns in enumerate(delays_ns):
        if t_ns <= 0.0:
            values.append(1.0)                        # ideal prep, no wait
            continue
        device = {"qubits": qubits,
                  "gates": {"id_0": {"duration_ns": t_ns, "error_rate": 0.0}}}
        device_model = qplayer.DeviceModel.from_json_string(json.dumps(device))
        noise_model = qplayer.NoiseModel(); noise_model.load_device(device_model)
        circuit = qplayer.Circuit(1, 1)
        if kind == "t1":
            circuit.x(0); circuit.id(0); circuit.measure(0, 0)
        else:
            circuit.h(0); circuit.id(0); circuit.h(0); circuit.measure(0, 0)
        simulator = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        # Independent seed per delay so the exp fit averages 16 uncorrelated
        # points (a shared seed correlates the shot error -> biased fit).
        simulator.set_seed(seed + i); simulator.set_noise_model(noise_model)
        values.append(relaxation_observable(simulator.run(circuit, shots).counts(), kind))
    return values


def measure_ibmq_relaxation(t1_ns, t2_ns, delays_ns, kind, shots, seed):
    """Aer's authentic thermal_relaxation_error channel. None if unavailable or
    T2 > 2*T1 (which thermal_relaxation_error rejects)."""
    try:
        from qiskit import QuantumCircuit
        from qiskit_aer import AerSimulator
        from qiskit_aer.noise import NoiseModel as AerNoiseModel
        from qiskit_aer.noise import thermal_relaxation_error
    except Exception:
        return None
    if not (t1_ns and t2_ns) or t2_ns > 2.0 * t1_ns:
        return None
    values = []
    for i, t_ns in enumerate(delays_ns):
        if t_ns <= 0.0:
            values.append(1.0)
            continue
        noise_model = AerNoiseModel()
        noise_model.add_quantum_error(thermal_relaxation_error(t1_ns, t2_ns, t_ns),
                                      ["id"], [0])
        circuit = QuantumCircuit(1, 1)
        if kind == "t1":
            circuit.x(0); circuit.id(0); circuit.measure(0, 0)
        else:
            circuit.h(0); circuit.id(0); circuit.h(0); circuit.measure(0, 0)
        simulator = AerSimulator(noise_model=noise_model)
        counts = simulator.run(circuit, shots=shots,
                               seed_simulator=seed + i).result().get_counts()
        values.append(relaxation_observable(counts, kind))
    return values


def fit_exponential_time_constant(delays_ns, ys, tau0_ns):
    """Fit ys ~ exp(-t/tau) (1-parameter, amplitude fixed at 1). Returns fitted
    tau in ns; falls back to tau0 if scipy/fit is unavailable."""
    try:
        import numpy as np
        from scipy.optimize import curve_fit
        t = np.asarray(delays_ns, float)
        y = np.asarray(ys, float)
        best, _ = curve_fit(lambda tt, tau: np.exp(-tt / tau), t, y,
                            p0=[float(tau0_ns)], bounds=(1.0, 1e13), maxfev=20000)
        return float(best[0])
    except Exception:
        return float(tau0_ns)


def plot_t1t2_decay(path, title, panels):
    """q0 amplitude-damping (T1) and dephasing (T2) decay vs idle time, two panels.
    Each panel overlays each engine's measured points (faint markers) with its
    exp(-t/T) fit (smooth line): QPlayer solid (T1 blue / T2 green), IBMQ dashed
    red. ``panels`` = list of dicts with x_us, qp, qp_tau_us, ib, ib_tau_us, color,
    name, ylabel, nominal_us. Returns path, or None if matplotlib absent."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return None

    one_over_e = 1.0 / math.e
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.3))
    for ax, panel in zip(axes, panels):
        xs = panel["x_us"]
        t_max = max(xs) if xs else 1.0
        dense = [t_max * k / 300.0 for k in range(301)]
        # QPlayer: measured markers + fitted curve.
        ax.plot(xs, panel["qp"], "o", color=panel["color"], ms=4, alpha=0.35)
        ax.plot(dense, [math.exp(-t / panel["qp_tau_us"]) for t in dense],
                color=panel["color"], lw=2.5, label="QPlayer")
        # IBMQ (Aer): measured markers + fitted curve.
        if panel["ib"] is not None:
            ax.plot(xs, panel["ib"], "x", color="#d1495b", ms=5, alpha=0.35)
            ax.plot(dense, [math.exp(-t / panel["ib_tau_us"]) for t in dense],
                    color="#d1495b", lw=1.6, ls="--", label="IBMQ")
        ax.axhline(one_over_e, color="#888", ls=":", lw=0.8)
        ax.axvline(panel["nominal_us"], ls=":", color="#888", lw=1)
        ax.text(panel["nominal_us"], one_over_e + 0.03, f" {panel['name']}",
                color="#888", fontsize=8)
        ax.set_xlabel("idle time (us)")
        ax.set_ylabel(panel["ylabel"])
        ax.set_title(f"{panel['title']} (fit {panel['name']}: "
                     f"QP={panel['qp_tau_us']:.1f}"
                     + (f", IBMQ={panel['ib_tau_us']:.1f}" if panel["ib"] is not None else "")
                     + f", nom={panel['nominal_us']:.1f} us)")
        ax.set_ylim(-0.02, 1.02)
        ax.set_xlim(0, t_max)
        ax.legend(fontsize=8); ax.grid(alpha=0.3)

    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return path


def distribution_plot_path(plot_dir, short, qasm):
    base = os.path.splitext(os.path.basename(qasm))[0]
    return os.path.join(plot_dir, f"{short}_{base}.png")


# =============================================================================
# SECTION 7 -- Finding a device (by short name or JSON path)
# =============================================================================

def resolve_device(device_arg):
    """Return (short_name, fake_class_name, json_path)."""
    if device_arg in DEVICES:
        fake_class, filename = DEVICES[device_arg]
        return device_arg, fake_class, os.path.join(DEVICE_JSON_DIR, filename)
    # treat as a path to a device JSON; class name comes from backend_name field
    if os.path.isfile(device_arg):
        with open(device_arg) as f:
            calibration = json.load(f)
        return os.path.basename(device_arg), calibration.get("backend_name"), device_arg
    raise SystemExit(f"unknown device '{device_arg}' "
                     f"(choose {list(DEVICES)} or a device JSON path)")


# =============================================================================
# SECTION 8 -- One (device, circuit) experiment
# -----------------------------------------------------------------------------
# This is the whole workflow for a single comparison, written top-to-bottom as
# numbered STEPs. It prints its report to stdout and (optionally) writes plots.
# =============================================================================

def run_one(device, qasm, shots, seed, max_sim_qubits, plot_dir, emit_t1t2=True):
    """Run one (device, circuit) combination, printing the report to stdout and
    (if plot_dir is given) writing plots. Returns a metadata dict describing what
    was produced (short name, transpiled width, whether tier 2 ran, plot paths)."""

    # ---- STEP 1: locate the device and load its calibration ----------------
    short, cls_name, json_path = resolve_device(device)
    with open(json_path) as f:
        calibration = json.load(f)

    meta = {"short": short, "cls_name": cls_name, "qasm": qasm,
            "width": None, "tier2": False, "t1t2_png": None, "dist_png": None}

    print(f"# Device-noise validation -- device={short} ({cls_name})  "
          f"circuit={os.path.basename(qasm)}")
    print(f"#   device JSON: {json_path}")
    print(f"#   shots={shots}  seed={seed}  max_sim_qubits={max_sim_qubits}")
    print()

    # ---- STEP 2: import the optional engines (skip gracefully if missing) --
    try:
        import qplayer
    except Exception as e:
        print(f"SKIP: qplayer not importable ({type(e).__name__}: {e})")
        return meta
    try:
        from qiskit import transpile
        from qiskit.qasm2 import load as qasm2_load
    except Exception as e:
        print(f"SKIP: qiskit not importable ({type(e).__name__}: {e})")
        return meta

    # The IBMQ engine is optional: without it the report keeps the QPlayer and
    # ideal columns and just omits the IBMQ reference.
    fake_backend = None
    aer_sim = None
    try:
        from qiskit_ibm_runtime import fake_provider
        from qiskit_aer import AerSimulator
        fake_backend = getattr(fake_provider, cls_name)()
        aer_sim = AerSimulator.from_backend(fake_backend)
    except Exception as e:
        print(f"# NOTE: FakeBackend reference unavailable ({type(e).__name__}: {e}); "
              f"the FakeBackend column will be omitted.")

    if plot_dir:
        os.makedirs(plot_dir, exist_ok=True)

    # ---- STEP 3: transpile the logical circuit onto the device -------------
    # Both IBMQ and QPlayer must run the identical circuit, so we transpile once
    # (device basis gates + coupling map) and reuse it for both engines.
    logical = qasm2_load(qasm)
    print(f"# Logical circuit: qubits={logical.num_qubits} "
          f"clbits={logical.num_clbits} gates=[{format_gate_counts(count_gates(logical))}]")

    if fake_backend is not None:
        transpiled = transpile(logical, backend=fake_backend,
                               optimization_level=1, seed_transpiler=seed)
    else:
        # No fake backend: transpile to the device basis only (no coupling map).
        transpiled = transpile(logical, basis_gates=["id", "rz", "sx", "x", "cx"],
                               optimization_level=1, seed_transpiler=seed)
    width = transpiled.num_qubits
    meta["width"] = width
    print(f"# Transpiled circuit: qubits={width} clbits={transpiled.num_clbits} "
          f"gates=[{format_gate_counts(count_gates(transpiled))}]")
    print()

    # ---- STEP 4 (TIER 1): coherence numbers for qubit 0 --------------------
    # Compare the T1/T2 times IBM reports against the ones QPlayer loads back.
    qubit0 = calibration["qubits"][0]
    device_model = qplayer.DeviceModel.from_json_string(json.dumps(calibration))
    ibmq_t1 = ibmq_t2 = ibmq_t2_star = None
    if fake_backend is not None:
        props = fake_backend.target.qubit_properties[0]
        ibmq_t1 = (props.t1 or 0.0) * 1e9
        ibmq_t2 = (props.t2 or 0.0) * 1e9
        ibmq_t2_star = getattr(props, "t2_star", None)
        ibmq_t2_star = (ibmq_t2_star * 1e9) if ibmq_t2_star else None

    print("## Tier 1 -- qubit 0 coherence parameters (ns)")
    print(f"{'param':<8} | {'IBMQ reported':>16} | {'QPlayer DeviceModel':>20}")
    print(f"{'-'*8}-+-{'-'*16}-+-{'-'*20}")

    def print_param_row(label, ibmq_value, qplayer_value):
        left = f"{ibmq_value:,.1f}" if isinstance(ibmq_value, float) else str(ibmq_value)
        right = f"{qplayer_value:,.1f}" if isinstance(qplayer_value, float) else str(qplayer_value)
        print(f"{label:<8} | {left:>16} | {right:>20}")

    print_param_row("T1", ibmq_t1 if ibmq_t1 else "N/A", device_model.T1(0))
    print_param_row("T2", ibmq_t2 if ibmq_t2 else "N/A", device_model.T2(0))
    print_param_row("T2*", ibmq_t2_star if ibmq_t2_star else "N/A (not in snapshot)",
                    "N/A (derived T_phi below)")
    t_phi = dephasing_time(device_model.T1(0), device_model.T2(0))
    print_param_row("T_phi", "-", t_phi if t_phi else "N/A")
    print(f"# readout q0: P(0|1)={qubit0.get('prob_meas0_prep1')}, "
          f"P(1|0)={qubit0.get('prob_meas1_prep0')}, "
          f"symmetric={qubit0.get('readout_error')}")
    print()

    # ---- STEP 5 (TIER 1b): per-qubit T1/T2 round-trip check ----------------
    # If the max difference is 0, QPlayer loaded every qubit's coherence exactly.
    num_qubits = int(calibration.get("num_qubits", len(calibration.get("qubits", []))))
    qplayer_t1 = [device_model.T1(q) for q in range(num_qubits)]
    qplayer_t2 = [device_model.T2(q) for q in range(num_qubits)]
    ibmq_t1_all = [None] * num_qubits
    ibmq_t2_all = [None] * num_qubits
    if fake_backend is not None:
        for q in range(num_qubits):
            props = fake_backend.target.qubit_properties[q]
            ibmq_t1_all[q] = (props.t1 or 0.0) * 1e9 if props.t1 else None
            ibmq_t2_all[q] = (props.t2 or 0.0) * 1e9 if props.t2 else None
        max_t1_diff = max((abs(a - b) for a, b in zip(ibmq_t1_all, qplayer_t1)
                           if a is not None and b not in (None, float("inf"))), default=0.0)
        max_t2_diff = max((abs(a - b) for a, b in zip(ibmq_t2_all, qplayer_t2)
                           if a is not None and b not in (None, float("inf"))), default=0.0)
        print(f"## Tier 1b -- per-qubit T1/T2 round-trip ({num_qubits} qubits)")
        print(f"# max |IBMQ - QPlayer|: T1={max_t1_diff:,.3f} ns, T2={max_t2_diff:,.3f} ns "
              f"(0 => DeviceModel loads device coherence exactly)")
        print()

    # ---- STEP 6 (TIER 1b plot): measured T1/T2 decay curves ----------------
    if plot_dir and emit_t1t2:
        # q0 T1/T2 relaxation sweep MEASURED on both engines' own channel, then
        # each measured curve is fit to exp(-t/T) for a smooth line + a fitted
        # time constant. 1 qubit -> cheap for every device width.
        t1_q0, t2_q0 = device_model.T1(0), device_model.T2(0)
        if t1_q0 and t2_q0 and math.isfinite(t1_q0) and math.isfinite(t2_q0):
            panels = []
            for kind, tau, color, name, ylabel, title in (
                    ("t1", t1_q0, "#2a7fbf", "T1", "P(|1>)", "Amplitude damping"),
                    ("t2", t2_q0, "#2a9d5a", "T2", "coherence <X>", "Dephasing")):
                delays = relaxation_time_grid(tau)
                qplayer_curve = measure_qplayer_relaxation(qplayer, calibration, delays,
                                                           kind, shots, seed)
                ibmq_curve = measure_ibmq_relaxation(t1_q0, t2_q0, delays, kind, shots, seed)
                panels.append({
                    "x_us": [d / 1000.0 for d in delays],
                    "qp": qplayer_curve,
                    "qp_tau_us": fit_exponential_time_constant(delays, qplayer_curve, tau) / 1000.0,
                    "ib": ibmq_curve,
                    "ib_tau_us": (fit_exponential_time_constant(delays, ibmq_curve, tau) / 1000.0)
                                 if ibmq_curve is not None else None,
                    "color": color, "name": name, "ylabel": ylabel,
                    "title": title, "nominal_us": tau / 1000.0,
                })
            t1t2_png = os.path.join(plot_dir, f"{short}_T1T2.png")
            written = plot_t1t2_decay(
                t1t2_png,
                f"{short} ({cls_name}) q0 -- T1/T2 relaxation "
                f"(measured on each engine's channel, exp(-t/T) fit)",
                panels)
            if written:
                meta["t1t2_png"] = written
                for panel in panels:
                    ibmq_text = (f" IBMQ={panel['ib_tau_us']:.1f}"
                                 if panel["ib"] is not None else " IBMQ=N/A")
                    print(f"# {panel['name']} fit (us): QPlayer={panel['qp_tau_us']:.1f}"
                          f"{ibmq_text}  nominal={panel['nominal_us']:.1f}")
                print(f"# t1t2_plot: {written}  (q0 measured decay + exp fit)")
                print()

    # ---- STEP 7 (TIER 2): measured-distribution comparison -----------------
    # Translate the transpiled circuit onto QPlayer, then let QPlayer's OWN
    # device-bound truncation shrink it (QubitTruncationPass): a small
    # circuit mapped onto a wide device (e.g. FakeGeneva 27q) references only a
    # few physical qubits, so truncation compresses it to a dense [0,k) range.
    # QPlayer then simulates 2^k (not 2^27), and run() projects the physical-keyed
    # device noise onto that range (project_to_dense), so each active qubit keeps
    # its own T1/T2 / gate error / readout. Aer prunes idle qubits internally, so
    # it keeps running the full transpiled circuit as the reference. We only
    # truncate when the device is wider than max_sim_qubits, leaving narrow
    # devices (manila/oslo) byte-identical to the direct path.
    qplayer_circuit = translate_circuit(transpiled, qplayer)
    sim_width = width
    if width > max_sim_qubits:
        truncator = qplayer.QRegister(qplayer.SimType.FullState, qplayer.ProcType.CPU)
        qplayer_circuit = truncator.transpile(qplayer_circuit, 0,
                                              json.dumps(calibration))
        sim_width = qplayer_circuit.num_qubits()
        meta["sim_width"] = sim_width
        print(f"# NOTE: device width {width}q exceeds max_sim_qubits "
              f"{max_sim_qubits}; QubitTruncationPass compresses the "
              f"circuit onto its {sim_width} active physical qubits (dense [0,k)), "
              f"so QPlayer simulates 2^{sim_width} and run() projects each qubit's "
              f"own device noise onto that range. Aer prunes idle qubits itself.")
        print()

    # Skip the (expensive) distribution tier only if even the active-qubit circuit
    # is too wide to simulate per-shot.
    if sim_width > max_sim_qubits:
        print(f"## Tier 2 -- distribution comparison: SKIPPED "
              f"(active width {sim_width}q > max_sim_qubits {max_sim_qubits}; "
              f"per-shot Monte-Carlo noisy simulation cost ~ shots x 2^{sim_width}).")
        print("# Parameter validation (Tier 1 / 1b) stands on its own for this device.")
        return meta

    # Width-aware shot count for the distribution tier: scale up with the active
    # state space so a spread distribution is sampled above the statistical floor
    # (see SHOTS_PER_STATE). The requested shots is a floor, so narrow circuits are
    # unchanged and only wide active widths (e.g. geneva's 12q ansatz) scale up.
    dist_shots = max(shots, SHOTS_PER_STATE * (1 << sim_width))
    meta["shots_req"] = shots        # requested floor
    meta["dist_shots"] = dist_shots  # effective shots actually sampled (>= floor)
    if dist_shots != shots:
        print(f"# NOTE: active width {sim_width}q -> distribution tier uses "
              f"{dist_shots} shots (>= {SHOTS_PER_STATE} x 2^{sim_width}) so the "
              f"spread distribution is sampled above the statistical floor "
              f"(requested shots={shots} is the floor).")
        print()

    # Run all three engines (see SECTION 5). QPlayer uses the (possibly truncated)
    # active-qubit circuit; Aer uses the full transpiled circuit.
    ideal = run_ideal_engine(qplayer, qplayer_circuit, dist_shots, seed)
    qplayer_noisy = run_qplayer_engine(qplayer, qplayer_circuit, calibration, dist_shots, seed)
    ibmq = run_ibmq_engine(aer_sim, transpiled, dist_shots, seed)

    # Line the noisy results up with the ideal one so bit-order differences do
    # not masquerade as disagreement.
    if ibmq is not None:
        ibmq, _ = match_bit_order(ideal, ibmq)
    qplayer_noisy, _ = match_bit_order(ideal, qplayer_noisy)
    meta["tier2"] = True

    # ---- STEP 8: print the distribution table ------------------------------
    print("## Tier 2 -- measured distribution (probabilities)")
    columns = ["ideal"]
    if ibmq is not None:
        columns.append("FakeBcknd")
    columns.append("QPlayer")
    header = f"{'state':>{max(5, sim_width)}} | " + " | ".join(f"{c:>9}" for c in columns)
    print(header)
    print("-" * len(header))
    all_outcomes = set(ideal)
    if ibmq:
        all_outcomes |= set(ibmq)
    all_outcomes |= set(qplayer_noisy)
    for state in sorted(all_outcomes, key=lambda s: -(ideal.get(s, 0)
                        + (ibmq.get(s, 0) if ibmq else 0)
                        + qplayer_noisy.get(s, 0)))[:16]:
        cells = [f"{ideal.get(state, 0.0):9.4f}"]
        if ibmq is not None:
            cells.append(f"{ibmq.get(state, 0.0):9.4f}")
        cells.append(f"{qplayer_noisy.get(state, 0.0):9.4f}")
        print(f"{state:>{max(5, sim_width)}} | " + " | ".join(cells))
    print()

    # ---- STEP 9: score the agreement between engines -----------------------
    fidelity_ideal_ibmq = hellinger_fidelity(ideal, ibmq) if ibmq is not None else None
    fidelity_ideal_qplayer = hellinger_fidelity(ideal, qplayer_noisy)
    fidelity_ibmq_qplayer = hellinger_fidelity(ibmq, qplayer_noisy) if ibmq is not None else None

    print("## Tier 2 -- distribution agreement")
    print(f"{'pair':<26} | {'Hellinger fidelity':>18} | {'TVD':>10}")
    print(f"{'-'*26}-+-{'-'*18}-+-{'-'*10}")
    if ibmq is not None:
        print(f"{'ideal      vs FakeBackend':<26} | {fidelity_ideal_ibmq:>18.5f} "
              f"| {total_variation_distance(ideal, ibmq):>10.5f}")
    print(f"{'ideal      vs QPlayer':<26} | {fidelity_ideal_qplayer:>18.5f} "
          f"| {total_variation_distance(ideal, qplayer_noisy):>10.5f}")
    if ibmq is not None:
        print(f"{'FakeBackend vs QPlayer (key)':<26} | {fidelity_ibmq_qplayer:>18.5f} "
              f"| {total_variation_distance(ibmq, qplayer_noisy):>10.5f}")
    print()
    if ibmq is not None:
        # Headline "% agreement" between QPlayer and the IBM device model
        # (FakeBackend). Two complementary readings of the same key row:
        #   overlap = 1 - TVD -> fraction of probability mass the two
        #             distributions literally share (most intuitive "% match");
        #   Hellinger fidelity -> statistical fidelity, the report's key metric.
        overlap = 1.0 - total_variation_distance(ibmq, qplayer_noisy)
        meta["agreement_overlap"] = overlap
        meta["agreement_hellinger"] = fidelity_ibmq_qplayer
        print(f"# QPlayer vs IBM (FakeBackend) agreement: "
              f"{overlap * 100:.2f}% distribution overlap (1 - TVD)  |  "
              f"{fidelity_ibmq_qplayer * 100:.2f}% Hellinger fidelity")
    print("# Higher Hellinger fidelity (-> 1.0) and lower TVD (-> 0.0) on the "
          "'FakeBackend vs QPlayer' row mean QPlayer reproduces the device noise "
          "more faithfully. ideal-vs-* rows quantify how far each noisy run "
          "departs from the noiseless distribution.")

    # ---- STEP 10: write the distribution comparison plot -------------------
    if plot_dir:
        written = plot_distribution_comparison(
            distribution_plot_path(plot_dir, short, qasm),
            f"{short} / {os.path.basename(qasm)}  (shots={shots})",
            ideal, ibmq, qplayer_noisy,
            {"ideal_ibmq": fidelity_ideal_ibmq, "ideal_qp": fidelity_ideal_qplayer,
             "ibmq_qp": fidelity_ibmq_qplayer})
        if written:
            meta["dist_png"] = written
            print(f"# plot: {written}")
    return meta


# =============================================================================
# SECTION 9 -- Environment info (recorded at the top of the report)
# =============================================================================

def _version(mod_name):
    try:
        import importlib
        return getattr(importlib.import_module(mod_name), "__version__", "?")
    except Exception:
        return None


def _cpu_model():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _qplayer_so_mtime():
    try:
        import qplayer
        package_dir = os.path.dirname(qplayer.__file__)
        shared_objects = glob.glob(os.path.join(package_dir, "_qplayer*.so"))
        if shared_objects:
            return datetime.fromtimestamp(os.path.getmtime(shared_objects[0])) \
                .strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        pass
    return None


def collect_env(shots, ansatz_shots, seed):
    rows = [
        ("host", f"{platform.system()} {platform.release()} {platform.machine()}"),
        ("cpu", _cpu_model()),
        ("qiskit", _version("qiskit") or "not installed"),
        ("qiskit-aer", _version("qiskit_aer") or "not installed"),
        ("qiskit-ibm-runtime", _version("qiskit_ibm_runtime") or "not installed"),
        ("matplotlib", _version("matplotlib") or "not installed"),
        ("scipy", _version("scipy") or "not installed"),
    ]
    qplayer_version = _version("qplayer")
    so_mtime = _qplayer_so_mtime()
    qplayer_cell = (qplayer_version or "not installed") + \
        (f" (.so built {so_mtime})" if so_mtime else "")
    rows.append(("qplayer", qplayer_cell))
    rows.append(("generic shots / ansatz shots", f"{shots} / {ansatz_shots}"))
    rows.append(("seed", str(seed)))
    return rows


# =============================================================================
# SECTION 10 -- Full-matrix sweep (--report) and markdown writer
# =============================================================================

def run_report(shots, ansatz_shots, seed, max_sim_qubits, out_dir):
    """Sweep the full device x circuit matrix and write a markdown report + PNGs.

    Matrix: for each device (manila, oslo, geneva) run the shared generic circuits
    (qasm/*.qasm) at ``shots``, then the device-specific ansatz circuits
    (qasm/<device>/*.qasm) at ``ansatz_shots``. Geneva's device width (27q) exceeds
    max_sim_qubits, so QubitTruncationPass compresses every geneva circuit
    onto its active physical qubits and QPlayer simulates just those (Tier 2 runs).
    Geneva's ansatz is partial-width (GENEVA_QUBITS, see gen_ansatz_qasm.py) rather
    than the full 27q so it stays simulable. shots / ansatz_shots are FLOORS: run_one
    scales the distribution tier up to SHOTS_PER_STATE * 2^(active width) so a wide,
    noise-spread distribution (e.g. geneva's 12q ansatz) is sampled above the
    statistical floor; narrow circuits stay at the requested value.
    """
    try:
        import qplayer  # noqa: F401
    except Exception as e:
        print(f"QPlayer is not importable ({type(e).__name__}: {e}); "
              f"nothing to validate. Install the qplayer package first.",
              file=sys.stderr)
        return 1

    stamp = datetime.now().strftime("%Y-%m-%d-%H%M")
    date_human = datetime.now().strftime("%Y-%m-%d %H:%M")
    os.makedirs(out_dir, exist_ok=True)
    plot_dir = os.path.join(out_dir, f"plots-{stamp}")

    generic_circuits = sorted(glob.glob(os.path.join(QASM_DIR, "*.qasm")))
    if not generic_circuits:
        print(f"No generic circuits found in {QASM_DIR}", file=sys.stderr)
        return 1

    # Build the list of runs. Each run is
    #   (device, qasm path, shots, is_first_circuit_for_this_device).
    # The "first" flag makes the T1/T2 decay plot emit once per device.
    runs = []
    for device in DEVICES:
        first = True
        for circuit in generic_circuits:
            runs.append((device, circuit, shots, first))
            first = False
        for circuit in sorted(glob.glob(os.path.join(QASM_DIR, device, "*.qasm"))):
            runs.append((device, circuit, ansatz_shots, False))

    print(f"Sweeping {len(runs)} (device, circuit) combination(s); "
          f"seed={seed}\nplots -> {plot_dir}\n")

    results = []  # (device, qasm name, captured text, meta)
    for device, circuit, run_shots, is_first in runs:
        name = os.path.basename(circuit)
        print(f"[run ] {device:<7} {name:<16} shots={run_shots}")
        captured = io.StringIO()
        try:
            with redirect_stdout(captured):
                meta = run_one(device, circuit, run_shots, seed, max_sim_qubits,
                               plot_dir, emit_t1t2=is_first)
        except Exception as e:  # noqa: BLE001 - record and continue
            captured.write(f"\nERROR: {type(e).__name__}: {e}\n")
            meta = {"short": device, "qasm": circuit, "width": None, "tier2": False,
                    "t1t2_png": None, "dist_png": None}
        results.append((device, name, captured.getvalue(), meta))

    report_path = os.path.join(out_dir, f"noise-validation-{stamp}.md")
    _write_report(report_path, date_human, shots, ansatz_shots, seed,
                  max_sim_qubits, results, plot_dir)
    print(f"\nReport written: {report_path}")
    return 0


def _write_report(path, date_human, shots, ansatz_shots, seed, max_sim_qubits,
                  results, plot_dir):
    plot_rel = os.path.relpath(plot_dir, os.path.dirname(path))
    lines = []
    add = lines.append
    add(f"# Device-noise validation report -- {date_human}")
    add("")
    add("> Tool: `comparison.py`")
    add("> Compared per (device, circuit): Ideal (QPlayer, no noise) · "
        "FakeBackend (Aer `from_backend`) · QPlayer (`load_device`)")
    add("> Devices: manila (5q) · oslo (7q) · geneva (27q; generic-circuit "
        "distributions simulated on the active qubits via QubitTruncationPass, "
        ")")
    add("> Tiers: (1) q0 T1/T2/T2*/T_phi/readout · (1b) per-qubit round-trip · "
        "(1b plot) measured T1/T2 decay with exp(-t/T) fit · "
        "(2) distribution + Hellinger fidelity / TVD")
    add("> Key metric: the `FakeBackend vs QPlayer` Hellinger fidelity "
        "(-> 1.0 = QPlayer reproduces the device noise faithfully)")
    add("> Agreement: per experiment, `QPlayer vs IBM (FakeBackend) agreement` "
        "reports distribution overlap (1 - TVD) and Hellinger fidelity as "
        "percentages; the Matrix summary lists Hellinger fidelity and TVD as "
        "raw [0, 1] values (see the note under that table).")
    add("")
    add("## Environment")
    add("| Item | Value |")
    add("|------|-------|")
    add(f"| datetime | {date_human} |")
    for key, value in collect_env(shots, ansatz_shots, seed):
        add(f"| {key} | {value} |")
    add(f"| max_sim_qubits | {max_sim_qubits} |")
    add("")
    add("## Matrix summary")
    add("| Device | Circuit | Transpiled width | Tier 2 (distribution) | "
        "Effective shots | Hellinger | TVD |")
    add("|--------|---------|-----------------:|:---------------------:|"
        "----------------:|----------:|----------:|")
    for device, name, _text, meta in results:
        width = meta.get("width")
        sim_width = meta.get("sim_width")
        width_text = str(width) if width is not None else "-"
        if sim_width is not None and sim_width != width:
            width_text = f"{width} → sim {sim_width}"
        tier2 = "✓" if meta.get("tier2") else "params-only"
        # Effective (possibly width-scaled) distribution shots; "↑" flags an
        # auto-scale above the requested floor (see SHOTS_PER_STATE).
        dist_shots = meta.get("dist_shots")
        shots_req = meta.get("shots_req")
        if dist_shots is None:
            shots_text = "-"
        elif shots_req is not None and dist_shots != shots_req:
            shots_text = f"{dist_shots} ↑ (floor {shots_req})"
        else:
            shots_text = str(dist_shots)
        # Two raw [0, 1] metrics side by side: Hellinger fidelity (-> 1.0) and
        # TVD (-> 0.0). TVD is derived from the stored overlap (overlap = 1 - TVD)
        # so the per-experiment scoring in run_one() stays untouched.
        overlap = meta.get("agreement_overlap")
        hellinger = meta.get("agreement_hellinger")
        if hellinger is not None and overlap is not None:
            hell_text = f"{hellinger:.4f}"
            tvd_text = f"{1.0 - overlap:.4f}"
        else:
            hell_text = tvd_text = "-"
        add(f"| {device} | {name} | {width_text} | {tier2} | {shots_text} | "
            f"{hell_text} | {tvd_text} |")
    add("")
    add("Both columns compare the **QPlayer noisy run against the FakeBackend** "
        "(the IBM device-noise reference), higher/lower being better as noted:")
    add("")
    add("- **Hellinger fidelity** `(sum_o sqrt(p_o * q_o))^2` in [0, 1] -- "
        "statistical similarity of the two measurement distributions. "
        "**1.0 = identical**; higher is better. Robust but saturates near 1.0, "
        "so small differences look compressed.")
    add("- **TVD** (total-variation distance) `0.5 * sum_o |p_o - q_o|` in "
        "[0, 1] -- how far apart the two distributions are. **0.0 = identical**; "
        "lower is better. A true distance, so it shows error changes with more "
        "contrast than Hellinger.")
    add("- Read both together with *Effective shots*: at a low shot count both "
        "metrics are dominated by sampling noise (two samples of the same "
        "distribution already disagree), not by model mismatch.")
    add("")

    # Group results per device so each device's T1/T2 plot is embedded once.
    add("## Results")
    seen_devices = set()
    for device, name, text, meta in results:
        if device not in seen_devices:
            seen_devices.add(device)
            add("")
            add(f"## Device: {device}")
            t1t2 = meta.get("t1t2_png")
            if t1t2 and os.path.exists(t1t2):
                rel = os.path.join(plot_rel, os.path.basename(t1t2))
                add(f"### {device} -- q0 T1/T2 relaxation (measured + exp fit)")
                add(f"![{device} T1/T2]({rel})")
                add("")
        add("")
        add(f"### {device} / {name}")
        add("```text")
        add(text.rstrip("\n"))
        add("```")
        dist = meta.get("dist_png")
        if dist and os.path.exists(dist):
            rel = os.path.join(plot_rel, os.path.basename(dist))
            add(f"![{device} {name} distribution]({rel})")
        add("")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


# =============================================================================
# SECTION 11 -- Command-line entry point
# =============================================================================

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="device-noise validation harness",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", nargs="?", default=None,
                    help="manila | oslo | geneva | <device.json> (console mode)")
    ap.add_argument("qasm", nargs="?", default=None,
                    help="path to the logical QASM circuit (console mode)")
    ap.add_argument("--report", action="store_true",
                    help="sweep the full device x circuit matrix and write a "
                         "markdown report + PNG plots into --out-dir")
    ap.add_argument("--out-dir", default=None,
                    help="report/plot output directory for --report "
                         "(default: ./result next to this script)")
    ap.add_argument("--shots", type=int, default=1024,
                    help="shot FLOOR for generic circuits (bell/ghz/x_relax); the "
                         "distribution tier auto-scales up to SHOTS_PER_STATE x "
                         "2^(active width) for wide circuits")
    ap.add_argument("--ansatz-shots", type=int, default=4096,
                    help="shot FLOOR for device-specific VQE/QAOA circuits "
                         "(--report); auto-scaled up for wide active widths "
                         "(e.g. geneva 12q -> SHOTS_PER_STATE x 2^12)")
    ap.add_argument("--seed", type=int, default=None,
                    help="Monte-Carlo / transpiler seed. Omit for a fresh random "
                         "seed (the chosen value is printed so any run stays "
                         "reproducible).")
    ap.add_argument("--max-sim-qubits", type=int, default=14,
                    help="skip the distribution tier above this transpiled width")
    ap.add_argument("--plot-dir", default=None,
                    help="console mode: write comparison PNGs into this dir "
                         "(requires matplotlib; no-op if unavailable)")
    args = ap.parse_args(argv)

    # No --seed => draw a fresh random seed so each experiment is independent of
    # prior runs. The value is recorded, so any single run stays reproducible
    # (re-run with --seed <printed value>).
    if args.seed is None:
        args.seed = int.from_bytes(os.urandom(4), "little")

    if args.report:
        out_dir = args.out_dir or RESULT_DIR
        return run_report(args.shots, args.ansatz_shots, args.seed,
                          args.max_sim_qubits, out_dir)

    if args.device is None or args.qasm is None:
        ap.error("provide <device> <circuit.qasm>, or use --report to sweep")

    run_one(args.device, args.qasm, args.shots, args.seed,
            args.max_sim_qubits, args.plot_dir, emit_t1t2=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
