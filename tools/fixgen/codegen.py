"""Venue spec -> C++.

For each venue we emit:
  <id>.hpp  typed tag / enum-value constants for hand-written code, plus accessors
  <id>.cpp  constexpr dictionary + mapping tables and an adapter factory
and once per build:
  registry.cpp  make_all_venues() / make_venue(id)

The generated code is data, not logic: behaviour lives in fixcore::SpecAdapter and
fixcore::Dictionary, so it is tested once rather than per venue.
"""
from __future__ import annotations

import json
from pathlib import Path

from jinja2 import Environment, FileSystemLoader, StrictUndefined

from .spec import field_map, load_and_validate

TEMPLATES = Path(__file__).parent / "templates"

FIELD_TYPES = {
    "STRING": "String", "CHAR": "Char", "INT": "Int", "SEQNUM": "SeqNum", "NUMINGROUP": "NumInGroup",
    "LENGTH": "Length", "QTY": "Qty", "PRICE": "Price", "PRICEOFFSET": "PriceOffset", "AMT": "Amt",
    "FLOAT": "Float", "PERCENTAGE": "Percentage", "BOOLEAN": "Boolean", "UTCTIMESTAMP": "UtcTimestamp",
    "UTCDATEONLY": "UtcDateOnly", "UTCTIMEONLY": "UtcTimeOnly", "LOCALMKTDATE": "LocalMktDate",
    "MONTHYEAR": "MonthYear", "CURRENCY": "Currency", "EXCHANGE": "Exchange",
    "MULTIPLEVALUESTRING": "MultipleValueString", "DATA": "Data",
}
DIRECTIONS = {"to_venue": "ToVenue", "from_venue": "FromVenue", "both": "Both"}


def cpp_str(s: str) -> str:
    """C++ string literal. json.dumps escaping is valid C++ for these inputs."""
    return json.dumps(s, ensure_ascii=True)


def _env() -> Environment:
    env = Environment(loader=FileSystemLoader(TEMPLATES), undefined=StrictUndefined,
                      keep_trailing_newline=True, trim_blocks=True, lstrip_blocks=True)
    env.filters["s"] = cpp_str
    return env


def build_model(spec: dict, spec_path: str) -> dict:
    """Flatten a validated spec into what the templates need."""
    fields = field_map(spec)
    tag = lambda name: fields[name]["tag"]

    messages = []
    for i, m in enumerate(spec["messages"]):
        rules = []
        for j, mf in enumerate(m["fields"]):
            conds = [{"tag": tag(cf), "values": vals, "id": f"m{i}_r{j}_c{k}"}
                     for k, (cf, vals) in enumerate(mf.get("required_when", {}).items())]
            rules.append({"tag": tag(mf["field"]), "required": bool(mf.get("required")),
                          "conds": conds, "id": f"m{i}_r{j}", "name": mf["field"]})
        messages.append({"msg_type": m["msg_type"], "name": m["name"],
                         "direction": DIRECTIONS[m["direction"]], "rules": rules, "id": f"m{i}"})

    oe = spec["order_entry"]
    s = spec["session"]
    tv = lambda d: [{"tag": tag(k), "value": v} for k, v in d.items()]
    nm = lambda d: [{"norm": k, "value": v} for k, v in d.items()]
    logon = []
    for lf in s.get("logon_fields", []):
        cred = lf.get("from", "").removeprefix("credential.")
        logon.append({"tag": tag(lf["field"]), "credential": cred, "value": lf.get("value", "")})

    return {
        "id": spec["venue"]["id"],
        "name": spec["venue"]["name"],
        "description": spec["venue"].get("description", ""),
        "spec_path": spec_path,
        "fields": [{**f, "cpp_type": FIELD_TYPES[f["type"]], "values": f.get("values", []),
                    "custom": bool(f.get("custom")), "description": f.get("description", "")}
                   for f in spec["fields"]],
        "messages": messages,
        "session": {"begin_string": s["begin_string"],
                    "default_appl_ver_id": s.get("default_appl_ver_id", ""),
                    "heartbeat": s.get("heartbeat_interval", 30),
                    "reset": "true" if s.get("reset_seq_num_on_logon", True) else "false",
                    "logon": logon},
        "instrument_tag": tag(oe["instrument"]["field"]),
        "instrument_extra": tv(oe["instrument"].get("extra", {})),
        "account_tag": tag(oe["account"]["field"]) if "account" in oe else 0,
        "account_required": "true" if oe.get("account", {}).get("required") else "false",
        "maps": {k: nm(oe.get(k, {})) for k in ("side", "ord_type", "time_in_force", "exec_type", "ord_status")},
        "defaults": tv(oe.get("defaults", {})),
        "cancel": "true" if oe.get("cancel") else "false",
        "replace": "true" if oe.get("replace") else "false",
    }


def generate(spec_paths: list[str | Path], out_dir: str | Path) -> list[Path]:
    out = Path(out_dir)
    (out / "fixgen" / "venues").mkdir(parents=True, exist_ok=True)
    env = _env()
    written: list[Path] = []
    venues = []

    def write(path: Path, text: str) -> None:
        # Only touch files whose content changed, so incremental builds stay incremental.
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
        written.append(path)

    for sp in sorted(Path(p) for p in spec_paths):
        spec = load_and_validate(sp)
        model = build_model(spec, sp.name)
        if model["id"] in {v["id"] for v in venues}:
            raise ValueError(f"duplicate venue id {model['id']}")
        venues.append(model)
        write(out / "fixgen" / "venues" / f"{model['id']}.hpp", env.get_template("venue.hpp.j2").render(v=model))
        write(out / f"{model['id']}.cpp", env.get_template("venue.cpp.j2").render(v=model))
    write(out / "registry.cpp", env.get_template("registry.cpp.j2").render(venues=venues))
    return written
