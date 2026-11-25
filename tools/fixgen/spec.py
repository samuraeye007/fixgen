"""Load, schema-validate and semantically lint venue specs."""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import jsonschema
import yaml

from . import SCHEMA_PATH

# Fields the core fills in itself from the normalized order model (or the session layer),
# so a venue requiring them does not mean the client has to supply them.
CORE_MAPPED = {"ClOrdID", "OrigClOrdID", "Side", "OrderQty", "OrdType", "Price",
               "TimeInForce", "TransactTime"}

REQUIRED_MSGS = {"D": "NewOrderSingle", "8": "ExecutionReport"}


@dataclass
class Issue:
    level: str  # "error" | "warning"
    path: str
    message: str

    def __str__(self) -> str:
        return f"{self.level.upper():7} {self.path}: {self.message}"


class SpecError(Exception):
    def __init__(self, issues: list[Issue]):
        self.issues = issues
        super().__init__("\n".join(str(i) for i in issues))


def load_schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text())


def load_spec(path: str | Path) -> dict:
    with open(path) as f:
        return yaml.safe_load(f)


def schema_issues(spec: dict) -> list[Issue]:
    validator = jsonschema.Draft202012Validator(load_schema())
    out = []
    for err in sorted(validator.iter_errors(spec), key=lambda e: list(e.absolute_path)):
        path = "/".join(str(p) for p in err.absolute_path) or "<root>"
        out.append(Issue("error", path, err.message))
    return out


def lint(spec: dict) -> list[Issue]:
    """Semantic checks the JSON schema cannot express. Assumes schema-valid input."""
    issues: list[Issue] = []
    err = lambda p, m: issues.append(Issue("error", p, m))
    warn = lambda p, m: issues.append(Issue("warning", p, m))

    fields: dict[str, dict] = {}
    tags: dict[int, str] = {}
    for i, f in enumerate(spec["fields"]):
        p = f"fields/{i}"
        if f["name"] in fields:
            err(p, f"duplicate field name {f['name']}")
        if f["tag"] in tags:
            err(p, f"tag {f['tag']} used by both {tags[f['tag']]} and {f['name']}")
        fields[f["name"]] = f
        tags[f["tag"]] = f["name"]
        if f.get("custom") and f["tag"] < 5000:
            warn(p, f"custom field {f['name']} has tag {f['tag']} below the user-defined range (5000+)")
        vals = [v["value"] for v in f.get("values", [])]
        if len(vals) != len(set(vals)):
            err(p, f"duplicate enum values in {f['name']}")

    def enum_values(name: str) -> set[str] | None:
        f = fields.get(name)
        if f is None or not f.get("values"):
            return None
        return {v["value"] for v in f["values"]}

    msgs: dict[str, dict] = {}
    for i, m in enumerate(spec["messages"]):
        p = f"messages/{i}"
        if m["msg_type"] in msgs:
            err(p, f"duplicate msg_type {m['msg_type']}")
        msgs[m["msg_type"]] = m
        names = [mf["field"] for mf in m["fields"]]
        for j, mf in enumerate(m["fields"]):
            fp = f"{p}/fields/{j}"
            if mf["field"] not in fields:
                err(fp, f"{m['name']} references undefined field {mf['field']}")
            if names.count(mf["field"]) > 1:
                err(fp, f"{mf['field']} listed twice in {m['name']}")
            for cond_field, cond_vals in mf.get("required_when", {}).items():
                if cond_field not in names:
                    err(fp, f"required_when refers to {cond_field}, which is not in {m['name']}")
                allowed = enum_values(cond_field)
                if allowed is not None:
                    for v in cond_vals:
                        if v not in allowed:
                            err(fp, f"required_when value {v!r} is not a valid {cond_field}")

    for mt, name in REQUIRED_MSGS.items():
        if mt not in msgs:
            err("messages", f"missing required message {name} (35={mt})")

    oe = spec["order_entry"]
    if oe.get("cancel") and "F" not in msgs:
        err("order_entry/cancel", "cancel enabled but no OrderCancelRequest (35=F)")
    if oe.get("replace") and "G" not in msgs:
        err("order_entry/replace", "replace enabled but no OrderCancelReplaceRequest (35=G)")
    if (oe.get("cancel") or oe.get("replace")) and "9" not in msgs:
        warn("messages", "no OrderCancelReject (35=9) defined; cancel rejects will be unparsed")

    def check_ref(path: str, name: str) -> None:
        if name not in fields:
            err(path, f"undefined field {name}")

    check_ref("order_entry/instrument/field", oe["instrument"]["field"])
    for k, v in oe["instrument"].get("extra", {}).items():
        check_ref(f"order_entry/instrument/extra/{k}", k)
        if (ev := enum_values(k)) is not None and v not in ev:
            err(f"order_entry/instrument/extra/{k}", f"{v!r} is not a valid {k}")
    if "account" in oe:
        check_ref("order_entry/account/field", oe["account"]["field"])
    for k, v in oe.get("defaults", {}).items():
        check_ref(f"order_entry/defaults/{k}", k)
        if (ev := enum_values(k)) is not None and v not in ev:
            err(f"order_entry/defaults/{k}", f"{v!r} is not a valid {k}")

    for key, fname in [("side", "Side"), ("ord_type", "OrdType"), ("time_in_force", "TimeInForce"),
                       ("exec_type", "ExecType"), ("ord_status", "OrdStatus")]:
        mapping = oe.get(key, {})
        if mapping and fname not in fields:
            err(f"order_entry/{key}", f"mapping given but field {fname} is not defined")
            continue
        ev = enum_values(fname)
        if ev is None:
            continue
        for norm, v in mapping.items():
            if v not in ev:
                err(f"order_entry/{key}/{norm}", f"{v!r} is not a valid {fname} value")
    if "limit" in oe["ord_type"] and "Price" not in fields:
        err("order_entry/ord_type", "limit orders supported but no Price field")

    for i, lf in enumerate(spec["session"].get("logon_fields", [])):
        check_ref(f"session/logon_fields/{i}", lf["field"])
    if spec["session"]["begin_string"] == "FIXT.1.1" and not spec["session"].get("default_appl_ver_id"):
        err("session", "FIXT.1.1 sessions need default_appl_ver_id")

    # Required NewOrderSingle fields nobody fills in become mandatory client "extras": just say so.
    if "D" in msgs:
        covered = set(CORE_MAPPED) | {oe["instrument"]["field"]} | set(oe["instrument"].get("extra", {})) \
            | set(oe.get("defaults", {}))
        if "account" in oe:
            covered.add(oe["account"]["field"])
        for mf in msgs["D"]["fields"]:
            if mf.get("required") and mf["field"] not in covered:
                warn("messages/D", f"required {mf['field']} has no mapping; clients must supply it as an extra")
    return issues


def validate(spec: dict) -> list[Issue]:
    issues = schema_issues(spec)
    if issues:
        return issues
    return lint(spec)


def load_and_validate(path: str | Path) -> dict:
    spec = load_spec(path)
    issues = [i for i in validate(spec) if i.level == "error"]
    if issues:
        raise SpecError(issues)
    return spec


def field_map(spec: dict) -> dict[str, dict[str, Any]]:
    return {f["name"]: f for f in spec["fields"]}
