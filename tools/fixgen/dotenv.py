"""Minimal .env loader (no dependency). KEY=value lines, '#' comments, optional quotes,
optional `export ` prefix. Variables already set in the environment win."""
from __future__ import annotations

import os
from pathlib import Path


def load_dotenv(*candidates: str | Path) -> Path | None:
    for c in candidates:
        path = Path(c)
        if not path.is_file():
            continue
        for raw in path.read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.removeprefix("export ").split("=", 1)
            key, value = key.strip(), value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            elif " #" in value:
                value = value.split(" #", 1)[0].rstrip()
            if key and key not in os.environ:
                os.environ[key] = value
        return path
    return None
