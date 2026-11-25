import copy
from pathlib import Path

import pytest

from fixgen.codegen import generate
from fixgen.spec import SpecError, lint, load_and_validate, load_spec, validate

ROOT = Path(__file__).resolve().parents[2]
SPECS = sorted((ROOT / "spec" / "venues").glob("*.yaml"))


@pytest.mark.parametrize("path", SPECS, ids=lambda p: p.stem)
def test_shipped_specs_are_valid(path):
    errors = [i for i in validate(load_spec(path)) if i.level == "error"]
    assert errors == []


def demo():
    return load_spec(ROOT / "spec" / "venues" / "demo_exch.yaml")


def errors_of(spec):
    return [str(i) for i in validate(spec) if i.level == "error"]


def test_lint_catches_undefined_field_reference():
    s = demo()
    s["messages"][0]["fields"].append({"field": "NoSuchField", "required": True})
    assert any("NoSuchField" in e for e in errors_of(s))


def test_lint_catches_mapping_to_value_not_in_enum():
    s = demo()
    s["order_entry"]["time_in_force"]["gtc"] = "1"  # TimeInForce only lists 0/3/4
    assert any("time_in_force/gtc" in e for e in errors_of(s))


def test_lint_catches_duplicate_tags_and_bad_conditions():
    s = demo()
    s["fields"].append({"tag": 11, "name": "Dup", "type": "STRING"})
    s["messages"][0]["fields"][-2]["required_when"] = {"OrdType": ["9"]}
    errs = errors_of(s)
    assert any("tag 11" in e for e in errs)
    assert any("'9' is not a valid OrdType" in e for e in errs)


def test_schema_rejects_soh_in_values():
    s = demo()
    s["order_entry"]["defaults"]["HandlInst"] = "1\x01"
    assert errors_of(s)


def test_replace_without_message_is_an_error():
    s = demo()
    s["messages"] = [m for m in s["messages"] if m["msg_type"] != "G"]
    assert any("replace" in e for e in errors_of(s))


def test_codegen_writes_files_and_is_stable(tmp_path):
    files = generate(SPECS, tmp_path)
    names = {f.name for f in files}
    assert {"registry.cpp", "demo_exch.cpp", "demo_exch.hpp"} <= names
    before = {f: f.stat().st_mtime_ns for f in files}
    generate(SPECS, tmp_path)
    assert all(f.stat().st_mtime_ns == m for f, m in before.items())  # untouched when unchanged
    hpp = (tmp_path / "fixgen" / "venues" / "acme_mtf.hpp").read_text()
    assert "inline constexpr int ClientCapacity = 20001;" in hpp
    assert 'inline constexpr std::string_view Principal = "P";' in hpp


def test_codegen_refuses_invalid_spec(tmp_path):
    s = demo()
    s["order_entry"]["side"]["buy"] = "X"
    import yaml
    p = tmp_path / "demo_exch.yaml"
    p.write_text(yaml.safe_dump(s))
    with pytest.raises(SpecError):
        generate([p], tmp_path / "out")


def test_quickfix_dictionaries(tmp_path):
    import xml.etree.ElementTree as ET

    generate(SPECS, tmp_path)
    d = tmp_path / "dict"
    assert {p.name for p in d.iterdir()} == {"demo_exch.xml", "acme_mtf.xml", "acme_mtf.transport.xml"}

    demo = ET.parse(d / "demo_exch.xml").getroot()
    assert (demo.get("type"), demo.get("major"), demo.get("minor")) == ("FIX", "4", "4")
    msgs = {m.get("msgtype"): m for m in demo.iter("message")}
    assert {"0", "1", "2", "3", "4", "5", "A", "D", "F", "G", "8", "9"} <= set(msgs)
    logon_fields = {f.get("name") for f in msgs["A"]}
    assert {"Username", "Password"} <= logon_fields
    # Lenient by design: no required app fields, no enum lists.
    assert all(f.get("required") == "N" for f in msgs["8"])
    assert not list(demo.iter("value"))

    transport = ET.parse(d / "acme_mtf.transport.xml").getroot()
    app = ET.parse(d / "acme_mtf.xml").getroot()
    assert transport.get("type") == "FIXT" and app.get("servicepack") == "2"
    assert {m.get("msgtype") for m in app.iter("message")} == {"D", "F", "8", "9"}
    assert "MemberCode" in {f.get("name") for f in transport.find("fields")}
