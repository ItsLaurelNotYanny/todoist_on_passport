#!/usr/bin/env python3
"""Acquire or verify explicit upstream source inputs; never execute unexpected downloads."""
import hashlib
import json
import os
from pathlib import Path
import sys
import urllib.request

root = Path(__file__).resolve().parents[1]
fetch = sys.argv[1:] == ["--fetch"]
if sys.argv[1:] not in ([], ["--fetch"]):
    raise SystemExit("Usage: dependencies.py [--fetch]")

lock_file = root / "dependencies.lock.json"
if not lock_file.exists():
    raise SystemExit("Missing dependencies.lock.json")

items = json.loads(lock_file.read_text(encoding="utf-8"))
for item in items:
    path = root / "vendor" / "sources" / item["file"]
    if not path.exists() and fetch:
        # Check optional local cache directory first to avoid re-downloading
        cache_dir = os.environ.get("SOURCE_CACHE_DIR")
        peer_sources = (Path(cache_dir) / item["file"]) if cache_dir else None
        if peer_sources and peer_sources.exists() and hashlib.sha256(peer_sources.read_bytes()).hexdigest() == item["sha256"]:
            path.parent.mkdir(parents=True, exist_ok=True)
            try:
                path.hardlink_to(peer_sources)
            except OSError:
                path.write_bytes(peer_sources.read_bytes())
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            req = urllib.request.Request(item["url"], headers={"User-Agent": "TodoistPassport-build/0.1"})
            with urllib.request.urlopen(req, timeout=45) as resp:
                data = resp.read(80 * 1024 * 1024 + 1)
            if len(data) > 80 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise SystemExit("Downloaded source digest mismatch: " + item["file"])
            path.write_bytes(data)

    if not path.exists():
        raise SystemExit("Missing pinned input; run python3 scripts/dependencies.py --fetch: " + item["file"])
    if hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
        raise SystemExit("Pinned input digest mismatch: " + item["file"])

print("Pinned source inputs verified")
