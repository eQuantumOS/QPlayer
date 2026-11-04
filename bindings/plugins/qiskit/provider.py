"""QPlayerProvider — discovers QPlayer backends.

A provider here is a plain duck-typed class exposing ``backends()`` /
``get_backend()`` — no base class to inherit. This runs unchanged on both qiskit
1.x (where Provider / ProviderV1 still exist but are deprecated — duck typing
avoids the deprecation warning) and qiskit 2.x (which removed those base classes
entirely). Registered under the ``qiskit.providers`` entry point so tooling can
discover it (see pyproject.toml).
"""
from __future__ import annotations

from .backend import QPlayerBackend


class QPlayerProvider:
    """Duck-typed Qiskit provider for QPlayer (no base class — qiskit 1.x and 2.x)."""

    def backends(self, name=None, **kwargs):
        """Return matching QPlayer backends (a single statevector backend)."""
        if name not in (None, "qplayer"):
            return []
        return [QPlayerBackend(provider=self, **kwargs)]

    def get_backend(self, name=None, **kwargs):
        """Return the QPlayer backend; ``engine``/``proc`` select sim type + device."""
        found = self.backends(name=name, **kwargs)
        if not found:
            raise QiskitBackendNotFoundError(f"no QPlayer backend named {name!r}")
        return found[0]


class QiskitBackendNotFoundError(Exception):
    """Raised by get_backend() when no matching backend exists."""
