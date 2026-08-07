#!/usr/bin/env python3
"""Source-tree adapter for the installed Action configuration generator."""

from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PYTHON_ROOT = REPO_ROOT / "python"
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

from hakoniwa_pdu_rpc.action_config_generator import main


if __name__ == "__main__":
    raise SystemExit(main())
