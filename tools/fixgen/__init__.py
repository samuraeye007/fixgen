"""fixgen tooling: venue spec validation, C++ codegen and PDF spec extraction."""
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "spec" / "schema" / "venue-spec.schema.json"
