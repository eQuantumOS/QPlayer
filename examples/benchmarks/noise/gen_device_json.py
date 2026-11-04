#!/usr/bin/env python3
"""gen_device_json.py -- Extract IBMQ fake-backend calibration into QPlayer
device-model JSON (device-noise validation harness).

Reads the snapshot calibration of three IBM Quantum fake backends and writes one
JSON file per device into ``device_json/``:

  - FakeManilaV2  (5 qubits)   -> fake_manila_v2.json
  - FakeOslo      (7 qubits)   -> fake_oslo.json
  - FakeGeneva    (27 qubits)  -> fake_geneva.json

All three are IBM Falcon processors whose native basis is ``{id, rz, sx, x, cx}``
(two-qubit gate = cx). The wide device (Geneva, 27q) exercises the parameter tier
and qubit-truncation path; the small/medium ones (Manila 5q, Oslo 7q) also
exercise the measured-distribution tier.

The JSON is a superset of QPlayer's DeviceModel schema. ``DeviceModel.from_json``
+ ``NoiseModel.load_device`` consume ALL of the noise-relevant fields directly:

  - qubits[].T1 / T2                              -> relaxation (amp damping + dephasing)
  - qubits[].prob_meas0_prep1 / prob_meas1_prep0  -> readout error (auto-registered)
  - gates["{gate}_{q...}"].duration_ns / error_rate -> per-gate relaxation +
        excess-over-relaxation depolarizing (1Q + 2Q), Qiskit ``from_backend`` style
  - basis_gates / two_qubit_gate                  -> basis-gate translation target
  - coupling_map                                  -> SWAP routing / coupling validation

So load_device on the committed JSON synthesises the FULL device noise
(relaxation + depolarizing + readout); comparison.py does not layer manual
channels on top. Extra keys (T2_star, frequency_hz) are ignored by the C++ parser
and read only by the report for display.

Units: qiskit Target reports times in SI SECONDS; QPlayer's DeviceModel uses
NANOSECONDS, so every duration / T1 / T2 is multiplied by 1e9 here.

Gate-key convention (matches DeviceModel make_gate_key): single-qubit gates ->
``"{gate}_{q}"`` (e.g. ``sx_0``); two-qubit gates -> ``"{gate}_{q0}_{q1}"``
(e.g. ``cx_0_1``). The per-qubit suffix lets load_device attach relaxation and
excess depolarizing to the exact operand.

This requires ``qiskit-ibm-runtime`` (the fake-backend provider) -- an optional
dependency. Run once; the produced JSON files are committed so the validation
harness works on the QPlayer side without the fake provider installed.

Run:  python gen_device_json.py            (writes all three)
      python gen_device_json.py manila     (one device by short name)
"""

import json
import os
import sys

S_TO_NS = 1.0e9  # qiskit Target uses SI seconds; QPlayer DeviceModel uses ns.

# Single-qubit basis gates whose relaxation/error we record. rz is a virtual
# (zero-duration, error-free) frame change on IBM hardware, so it carries no
# noise and is intentionally omitted from the gate table.
ONE_Q_GATES = ("sx", "x", "id")
TWO_Q_GATES = ("cx", "ecr", "cz")  # first present in the target wins

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "device_json")

# short name -> (FakeBackend class name, output file). FakeManilaV2 keeps the V2
# suffix (it has a V1 twin); Oslo/Geneva were added as BackendV2 directly, so no
# suffix. All three expose the ``.target`` V2 calibration this script reads.
DEVICES = {
    "manila": ("FakeManilaV2", "fake_manila_v2.json"),
    "oslo":   ("FakeOslo",     "fake_oslo.json"),
    "geneva": ("FakeGeneva",   "fake_geneva.json"),
}


def _finite_ns(value):
    """Convert an SI-second time to ns, or None if absent/non-finite."""
    if value is None:
        return None
    ns = float(value) * S_TO_NS
    return ns if ns > 0.0 else None


def _inst_prop(target, gate, key):
    """Return (duration_ns, error) for target[gate][key], guarding None/missing."""
    try:
        props = target[gate][key]
    except (KeyError, TypeError):
        return None, None
    dur = _finite_ns(getattr(props, "duration", None))
    err = getattr(props, "error", None)
    err = float(err) if err is not None else None
    return dur, err


def _readout_error(backend, target, q):
    """Asymmetric readout error for qubit q. Prefer the BackendProperties
    (prob_meas0_prep1 / prob_meas1_prep0) when the snapshot exposes them, else
    fall back to the symmetric Target measure error."""
    p01 = p10 = None
    props = getattr(backend, "properties", None)
    if callable(props):
        try:
            bp = backend.properties()
            if bp is not None:
                p01 = bp.qubit_property(q, "prob_meas0_prep1")[0]
                p10 = bp.qubit_property(q, "prob_meas1_prep0")[0]
        except Exception:
            p01 = p10 = None
    _, sym = _inst_prop(target, "measure", (q,))
    sym = sym if sym is not None else 0.0
    if p01 is None:
        p01 = sym
    if p10 is None:
        p10 = sym
    return float(p01), float(p10), float(sym)


def build_device_dict(backend):
    target = backend.target
    n = backend.num_qubits

    qubits = []
    for q in range(n):
        qp = target.qubit_properties[q]
        t1 = _finite_ns(getattr(qp, "t1", None))
        t2 = _finite_ns(getattr(qp, "t2", None))
        # t2_star is not part of the fake-backend snapshot; record None so the
        # report can show "N/A (not in snapshot)" rather than inventing a value.
        t2_star = _finite_ns(getattr(qp, "t2_star", None))
        p01, p10, sym = _readout_error(backend, target, q)
        qubits.append({
            "T1": t1,
            "T2": t2,
            "T2_star": t2_star,
            "frequency_hz": float(getattr(qp, "frequency", 0.0) or 0.0),
            "readout_error": sym,
            "prob_meas0_prep1": p01,
            "prob_meas1_prep0": p10,
        })

    gates = {}
    # Single-qubit gate table (per qubit).
    for gate in ONE_Q_GATES:
        if gate not in target.operation_names:
            continue
        for key in target[gate].keys():
            if key is None or len(key) != 1:
                continue
            (q,) = key
            dur, err = _inst_prop(target, gate, key)
            if dur is None and err is None:
                continue
            gates[f"{gate}_{q}"] = {
                "duration_ns": dur if dur is not None else 0.0,
                "error_rate": err if err is not None else 0.0,
            }
    # Two-qubit gate table (per directed pair) -- first available 2q gate.
    two_q = next((g for g in TWO_Q_GATES if g in target.operation_names), None)
    if two_q is not None:
        for key in target[two_q].keys():
            if key is None or len(key) != 2:
                continue
            q0, q1 = key
            dur, err = _inst_prop(target, two_q, key)
            if dur is None and err is None:
                continue
            gates[f"{two_q}_{q0}_{q1}"] = {
                "duration_ns": dur if dur is not None else 0.0,
                "error_rate": err if err is not None else 0.0,
            }

    coupling = []
    cm = target.build_coupling_map()
    if cm is not None:
        coupling = [[int(a), int(b)] for a, b in cm.get_edges()]

    return {
        "backend_name": type(backend).__name__,
        "num_qubits": int(n),
        "basis_gates": sorted(target.operation_names),
        "two_qubit_gate": two_q,
        "_units": "T1/T2/T2_star/duration_ns in nanoseconds; frequency_hz in Hz",
        "qubits": qubits,
        "gates": gates,
        "coupling_map": coupling,
    }


def main(argv):
    try:
        from qiskit_ibm_runtime import fake_provider
    except ImportError:
        print("ERROR: qiskit-ibm-runtime not installed (fake-backend provider). "
              "Install it (optional benchmark dependency):\n"
              "  pip install qiskit-ibm-runtime", file=sys.stderr)
        return 2

    selected = argv[1:] if len(argv) > 1 else list(DEVICES.keys())
    os.makedirs(OUT_DIR, exist_ok=True)

    for short in selected:
        if short not in DEVICES:
            print(f"unknown device '{short}' (choose from {list(DEVICES)})",
                  file=sys.stderr)
            return 2
        cls_name, out_name = DEVICES[short]
        cls = getattr(fake_provider, cls_name)
        backend = cls()
        device = build_device_dict(backend)
        out_path = os.path.join(OUT_DIR, out_name)
        with open(out_path, "w") as f:
            json.dump(device, f, indent=2)
            f.write("\n")
        q0 = device["qubits"][0]
        print(f"wrote {out_path}: {device['num_qubits']}q, "
              f"{len(device['gates'])} gate entries, "
              f"{len(device['coupling_map'])} couplings; "
              f"basis={device['basis_gates']}; 2q={device['two_qubit_gate']}; "
              f"q0 T1={q0['T1']:.1f} ns T2={q0['T2']:.1f} ns "
              f"readout={q0['readout_error']:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
