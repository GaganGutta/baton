"""A process that races others for ledger keys.

usage: ledger_script.py <ledger-path> <key-count> <name>

Prints "won <key>" after each put_if_absent() that returned True - that is,
after the entry is durable - and flushes, so a parent that kills this process
knows exactly what it had been promised.
"""

import sys
from pathlib import Path

try:
    from baton.idempotent import Ledger
except ImportError:  # not installed: use the source tree
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    from baton.idempotent import Ledger

ledger = Ledger(sys.argv[1])
for i in range(int(sys.argv[2])):
    key = f"key-{i}"
    if ledger.put_if_absent(key, {"by": sys.argv[3]}):
        print(f"won {key}", flush=True)
