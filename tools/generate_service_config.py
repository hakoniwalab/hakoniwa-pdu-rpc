#!/usr/bin/env python3
"""Source-tree entry point for the installed Service config generator."""

from __future__ import annotations

import sys
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1] / "python"
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

from hakoniwa_pdu_rpc.service_config_generator import main


if __name__ == "__main__":
    raise SystemExit(main())
