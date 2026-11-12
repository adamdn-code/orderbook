"""Shared setup: find the compiled `obfast` module and the pure-Python book."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "cpp" / "build"))  # obfast*.so
sys.path.insert(0, str(ROOT))  # pure-Python orderbook.py

try:
    import obfast  # noqa: E402
except ImportError as exc:  # pragma: no cover - only hit when not built
    raise SystemExit(
        "obfast isn't built yet. Run:  make -C cpp python\n"
        "(needs Python headers: sudo apt install python3-dev)"
    ) from exc

BUY, SELL = obfast.BUY, obfast.SELL

__all__ = ["obfast", "BUY", "SELL", "ROOT"]
