"""Matplotlib-native circuit diagram renderer.

Pure-Python image renderer for ``qplayer.Circuit``. Invoked from the C++ binding
``Circuit.draw(output="mpl", ...)`` which defers here so the C++/CUDA core stays
free of any graphics dependency. The renderer is backend-agnostic: it reads the
circuit through ``Circuit.to_json()`` and never touches a simulator.

matplotlib is an **optional** dependency (the ``qplayer[viz]`` extra). It is
imported lazily inside :func:`draw_mpl` so that ``import qplayer`` works without
it; a missing install raises a clear :class:`ImportError` pointing at the extra.
"""

from __future__ import annotations

import json

# Gates whose first qubit is a control marker (filled dot) with the remaining
# qubit carrying the boxed operation label. Mirrors the C++ text renderer roles.
_CONTROLLED_1T = {       # name -> target label drawn in a box
    "cx": "X", "cy": "Y", "ch": "H",
    "crz": "RZ", "cu1": "U1", "cu3": "U3",
}


def _fmt_params(params):
    if not params:
        return ""
    return "(" + ",".join(f"{p:g}" for p in params) + ")"


def _gate_label(node):
    name = node.get("opaque_name") or node.get("name", "?")
    return name.upper() + _fmt_params(node.get("params", []))


def _op_qubits(node):
    """Qubits an op touches (for layering / span)."""
    t = node["type"]
    if t == "gate":
        return list(node.get("qubits", []))
    if t == "measure":
        return [node["qubit"]]
    if t == "barrier":
        return list(node.get("qubits", []))
    if t == "if":
        return list(node["body"].get("qubits", []))
    return []  # idle: not drawn


def _assign_layers(ops, n):
    """Greedy column packing identical in spirit to the C++ text renderer:
    an op reserves its contiguous qubit span; measure / conditional ops reserve
    down to the last qubit so their drop to the classical wire never collides."""
    next_free = [0] * n
    layers = []
    for node in ops:
        qs = _op_qubits(node)
        if not qs and node["type"] != "if":
            layers.append(-1)
            continue
        lo, hi = min(qs), max(qs)
        if node["type"] in ("measure", "if"):
            hi = n - 1
        layer = max(next_free[lo:hi + 1]) if hi >= lo else 0
        for q in range(lo, hi + 1):
            next_free[q] = layer + 1
        layers.append(layer)
    return layers


def draw_mpl(circuit, style="unicode", filename=None):
    """Render *circuit* to a matplotlib ``Figure``.

    Parameters
    ----------
    circuit : qplayer.Circuit
    style : str
        Accepted for signature parity with the text renderer; the mpl output is
        visual so the value is currently ignored.
    filename : str | None
        If given, the figure is saved to this path (format inferred by suffix).

    Returns the ``matplotlib.figure.Figure``. Raises ImportError if matplotlib
    is not installed (install ``qplayer[viz]``).
    """
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Circle, Rectangle
    except ImportError as exc:  # pragma: no cover - exercised only without mpl
        raise ImportError(
            "Circuit.draw(output='mpl') requires matplotlib. "
            "Install it with:  pip install 'qplayer[viz]'   (or  pip install matplotlib)."
        ) from exc

    spec = json.loads(circuit.to_json())
    n = int(spec["num_qubits"])
    ops = spec.get("ops", [])
    if n == 0:
        fig, ax = plt.subplots(figsize=(2, 1))
        ax.text(0.5, 0.5, "(empty circuit)", ha="center", va="center")
        ax.axis("off")
        if filename:
            fig.savefig(filename, bbox_inches="tight")
        return fig

    layers = _assign_layers(ops, n)
    n_layers = max([0] + [L + 1 for L in layers if L >= 0])
    has_classical = any(node["type"] in ("measure", "if") for node in ops)

    # Geometry: x = layer (0..n_layers), qubit q drawn at y = (n-1-q) so q0 is
    # on top; the classical wire sits below qubit n-1 at y = -1.
    def yq(q):
        return n - 1 - q
    y_classical = -1.0

    fig_w = max(3.0, 1.1 * (n_layers + 1))
    fig_h = max(1.5, 0.7 * (n + (1 if has_classical else 0)))
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))

    box_blue = "#cfe3f7"
    edge = "#1a3b5d"

    # Qubit wires + labels.
    for q in range(n):
        ax.plot([-0.6, n_layers - 0.1], [yq(q), yq(q)], color="black", lw=1.0, zorder=1)
        ax.text(-0.7, yq(q), f"q{q}", ha="right", va="center", fontsize=10)
    if has_classical:
        ax.plot([-0.6, n_layers - 0.1], [y_classical + 0.04, y_classical + 0.04],
                color="0.4", lw=1.0, zorder=1)
        ax.plot([-0.6, n_layers - 0.1], [y_classical - 0.04, y_classical - 0.04],
                color="0.4", lw=1.0, zorder=1)
        ax.text(-0.7, y_classical, "c", ha="right", va="center", fontsize=10, color="0.3")

    def box(x, q, label, fc=box_blue):
        w = max(0.5, 0.16 * len(label) + 0.34)
        ax.add_patch(Rectangle((x - w / 2, yq(q) - 0.3), w, 0.6,
                               facecolor=fc, edgecolor=edge, lw=1.2, zorder=3))
        ax.text(x, yq(q), label, ha="center", va="center", fontsize=9, zorder=4)

    def ctrl(x, q):
        ax.add_patch(Circle((x, yq(q)), 0.09, facecolor=edge, edgecolor=edge, zorder=4))

    def target_x(x, q):
        ax.add_patch(Circle((x, yq(q)), 0.20, facecolor="white", edgecolor=edge, lw=1.4, zorder=3))
        ax.plot([x - 0.20, x + 0.20], [yq(q), yq(q)], color=edge, lw=1.2, zorder=4)
        ax.plot([x, x], [yq(q) - 0.20, yq(q) + 0.20], color=edge, lw=1.2, zorder=4)

    def swap_mark(x, q):
        d = 0.13
        ax.plot([x - d, x + d], [yq(q) - d, yq(q) + d], color=edge, lw=1.6, zorder=4)
        ax.plot([x - d, x + d], [yq(q) + d, yq(q) - d], color=edge, lw=1.6, zorder=4)

    def vline(x, qa, qb):
        ax.plot([x, x], [yq(qa), yq(qb)], color=edge, lw=1.2, zorder=2)

    def cdrop(x, q):  # vertical drop from qubit q to the classical wire
        ax.plot([x, x], [yq(q), y_classical], color="0.4", lw=1.0, ls=(0, (4, 2)), zorder=2)

    for node, L in zip(ops, layers):
        if L < 0:
            continue
        x = float(L)
        t = node["type"]

        if t == "barrier":
            qs = node.get("qubits", []) or list(range(n))
            ax.plot([x, x], [yq(max(qs)) - 0.45, yq(min(qs)) + 0.45],
                    color="0.55", lw=6, alpha=0.25, zorder=1)
            continue

        if t == "measure":
            box(x, node["qubit"], "M", fc="#eeeeee")
            cdrop(x, node["qubit"])
            ax.text(x + 0.12, y_classical, str(node["cubit"]),
                    ha="left", va="center", fontsize=8, color="0.3")
            continue

        gate = node["body"] if t == "if" else node
        name = gate.get("name", "")
        qs = gate.get("qubits", [])

        if t == "if":
            cdrop(x, min(qs))
            ax.text(x + 0.12, y_classical, "=" + str(node["value"]),
                    ha="left", va="center", fontsize=8, color="0.3")

        if name == "swap" and len(qs) == 2:
            vline(x, qs[0], qs[1]); swap_mark(x, qs[0]); swap_mark(x, qs[1])
        elif name == "cz" and len(qs) == 2:
            vline(x, qs[0], qs[1]); ctrl(x, qs[0]); ctrl(x, qs[1])
        elif name in _CONTROLLED_1T and len(qs) == 2:
            vline(x, qs[0], qs[1]); ctrl(x, qs[0])
            lbl = _CONTROLLED_1T[name] + _fmt_params(gate.get("params", []))
            if name == "cx":
                target_x(x, qs[1])
            else:
                box(x, qs[1], lbl)
        elif name == "ccx" and len(qs) == 3:
            vline(x, min(qs), max(qs)); ctrl(x, qs[0]); ctrl(x, qs[1]); target_x(x, qs[2])
        elif name == "cswap" and len(qs) == 3:
            vline(x, min(qs), max(qs)); ctrl(x, qs[0]); swap_mark(x, qs[1]); swap_mark(x, qs[2])
        else:
            # Single-qubit gate, reset, or generic / multi-qubit opaque.
            label = "|0>" if gate.get("type") == "gate" and name == "reset" else _gate_label(gate)
            if len(qs) > 1:
                vline(x, min(qs), max(qs))
            for q in qs:
                box(x, q, label)

    ax.set_xlim(-1.4, n_layers + 0.2)
    ax.set_ylim(y_classical - 0.6 if has_classical else -0.6, n - 0.4)
    ax.axis("off")
    ax.set_aspect("equal", adjustable="box")
    fig.tight_layout()

    if filename:
        fig.savefig(filename, bbox_inches="tight")
    return fig
