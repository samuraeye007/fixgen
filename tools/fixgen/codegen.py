"""Venue spec -> C++.

For each venue we emit:
  <id>.hpp  typed tag / enum-value constants for hand-written code, plus accessors
  <id>.cpp  constexpr dictionary + mapping tables and an adapter factory
  dict/<id>.xml (+ dict/<id>.transport.xml for FIXT)  QuickFIX data dictionaries
and once per build:
  registry.cpp  make_all_venues() / make_venue(id)

The generated code is data, not logic: behaviour lives in fixcore::SpecAdapter and
fixcore::Dictionary, so it is tested once rather than per venue.
"""
from __future__ import annotations

import json
from pathlib import Path

from jinja2 import Environment, FileSystemLoader, StrictUndefined

from .fix_standard import ADMIN_MESSAGES, APPL_VER_IDS, FIXT_HEADER_EXTRA, HEADER, SESSION_FIELDS, TRAILER
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


def quickfix_dictionaries(spec: dict) -> dict[str, tuple[str, dict]]:
    """QuickFIX data dictionaries for a venue: {file name: (purpose, model)}.

    QuickFIX only validates *incoming* messages, and a message that fails is answered with
    a session Reject and never reaches the application. If the venue sends a value our spec
    missed, a strict dictionary would silently drop that execution report. So these are
    deliberately lenient parsing dictionaries (types and message layout, nothing required,
    no enum lists); fixcore::Dictionary does the strict checks: it blocks bad outbound
    messages and turns inbound deviations into warnings on the order.
    """
    venue_id = spec["venue"]["id"]
    fields = field_map(spec)
    by_tag = {f["tag"]: f for f in spec["fields"]}
    for tag, (name, _) in SESSION_FIELDS.items():
        if tag in by_tag and by_tag[tag]["name"] != name:
            raise ValueError(f"{venue_id}: tag {tag} is {name} in FIX but {by_tag[tag]['name']} in the spec")
        if name in fields and fields[name]["tag"] != tag:
            raise ValueError(f"{venue_id}: field name {name} must be tag {tag}")

    def fdef(tag: int, name: str, typ: str) -> dict:
        return {"tag": tag, "name": name, "type": typ}

    session_fields = {t: fdef(t, n, ty) for t, (n, ty) in SESSION_FIELDS.items()}
    venue_fields = {f["tag"]: fdef(f["tag"], f["name"], f["type"]) for f in spec["fields"]}
    s = spec["session"]
    fixt = s["begin_string"] == "FIXT.1.1"
    logon_extra = [lf["field"] for lf in s.get("logon_fields", [])]

    def req(items):
        return [{"name": n, "required": r} for n, r in items]

    admin = []
    for mt, (name, flds) in ADMIN_MESSAGES.items():
        flds = list(flds)
        if mt == "A":
            if fixt:
                flds.insert(2, ("DefaultApplVerID", True))
            flds += [(n, False) for n in logon_extra if n not in {f for f, _ in flds}]
        admin.append({"msg_type": mt, "name": name, "cat": "admin", "fields": req(flds)})
    app = [{"msg_type": m["msg_type"], "name": m["name"], "cat": "app",
            "fields": [{"name": mf["field"], "required": False} for mf in m["fields"]]}
           for m in spec["messages"]]

    header = req(HEADER + (FIXT_HEADER_EXTRA if fixt else []))
    if not fixt:
        major, minor = s["begin_string"].removeprefix("FIX.").split(".")
        all_fields = {**session_fields, **venue_fields}
        d = {"type": "FIX", "major": major, "minor": minor, "sp": 0, "header": header, "trailer": req(TRAILER),
             "messages": admin + app, "fields": [all_fields[t] for t in sorted(all_fields)]}
        return {f"{venue_id}.xml": ("Session + application dictionary (lenient, see codegen.py).", d)}

    logon_tags = {fields[n]["tag"] for n in logon_extra}
    transport_fields = {**session_fields, **{t: venue_fields[t] for t in logon_tags}}
    major, minor, sp = APPL_VER_IDS.get(s.get("default_appl_ver_id", "9"), (5, 0, 2))
    transport = {"type": "FIXT", "major": 1, "minor": 1, "sp": 0, "header": header, "trailer": req(TRAILER),
                 "messages": admin, "fields": [transport_fields[t] for t in sorted(transport_fields)]}
    app_d = {"type": "FIX", "major": major, "minor": minor, "sp": sp, "header": [], "trailer": [],
             "messages": app, "fields": [venue_fields[t] for t in sorted(venue_fields)]}
    return {f"{venue_id}.transport.xml": ("FIXT.1.1 transport dictionary.", transport),
            f"{venue_id}.xml": ("Application dictionary (lenient, see codegen.py).", app_d)}


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
        (out / "dict").mkdir(exist_ok=True)
        for name, (purpose, d) in quickfix_dictionaries(spec).items():
            write(out / "dict" / name, env.get_template("quickfix_dict.xml.j2").render(
                d=d, purpose=purpose, spec_path=sp.name))
    write(out / "registry.cpp", env.get_template("registry.cpp.j2").render(venues=venues))
    return written
