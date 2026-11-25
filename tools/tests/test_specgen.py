"""specgen without the network: a fake extractor stands in for Claude, so these tests
cover splitting, merging, mapping derivation, validation and the hand-off to codegen."""
import json
from types import SimpleNamespace

import pytest
import yaml

pypdf = pytest.importorskip("pypdf")

from fixgen.codegen import generate  # noqa: E402
from fixgen.spec import validate  # noqa: E402
from fixgen.specgen.extract import ClaudeExtractor, ExtractionError, run, split_pdf  # noqa: E402
from fixgen.specgen.schema import CHUNK_SCHEMA  # noqa: E402


def blank_pdf(path, pages):
    w = pypdf.PdfWriter()
    for _ in range(pages):
        w.add_blank_page(width=612, height=792)
    with open(path, "wb") as f:
        w.write(f)
    return path


def f(tag, name, typ, values=(), pages=(1,), custom=False):
    return {"tag": tag, "name": name, "type": typ, "custom": custom, "description": "",
            "values": [{"value": v, "name": n, "description": ""} for v, n in values], "pages": list(pages)}


def mf(tag, name, required=True, when=()):
    return {"field": name, "tag": tag, "required": required,
            "required_when": [{"field": k, "values": v} for k, v in when], "note": ""}


EMPTY_SESSION = {"begin_string": None, "default_appl_ver_id": None, "heartbeat_interval": None,
                 "reset_seq_num_on_logon": None, "logon_fields": []}

# What a model might plausibly return for two overlapping chunks of a small spec.
CHUNK_1 = {
    "session": {**EMPTY_SESSION, "begin_string": "FIX.4.4", "heartbeat_interval": 30,
                "logon_fields": [{"field": "Username", "tag": 553, "credential": "username", "value": None, "pages": [2]},
                                 {"field": "Password", "tag": 554, "credential": "password", "value": None, "pages": [2]}]},
    "fields": [
        f(21, "HandlInst", "CHAR", [("1", "Automated")]),
        f(22, "SecurityIDSource", "STRING", [("4", "ISIN")]),
        f(40, "OrdType", "CHAR", [("1", "Market"), ("2", "Limit")]),
        f(54, "Side", "CHAR", [("1", "Buy"), ("2", "Sell")]),
        f(59, "TimeInForce", "CHAR", [("0", "Day"), ("3", "IOC"), ("9", "VenueSpecialAuction")]),
        f(9001, "Capacity", "CHAR", [("A", "Agency"), ("P", "Principal")], custom=True),
        f(34, "MsgSeqNum", "SEQNUM"),  # header field: must be dropped
    ],
    "messages": [
        {"msg_type": "A", "name": "Logon", "direction": "both", "fields": [mf(553, "Username")], "pages": [2]},
        {"msg_type": "D", "name": "New Order Single", "direction": "to_venue", "pages": [3, 4], "fields": [
            mf(11, "ClOrdID"), mf(21, "HandlInst"), mf(48, "SecurityID"), mf(22, "SecurityIDSource"),
            mf(54, "Side"), mf(38, "OrderQty"),
        ]},
    ],
    "notes": ["Table 4 lists Price as optional but the text says it is required for limit orders."],
}
CHUNK_2 = {
    "session": EMPTY_SESSION,
    "fields": [
        f(54, "Side", "CHAR", [("1", "Buy"), ("2", "Sell"), ("5", "SellShort")], pages=(5,)),
        f(150, "ExecType", "CHAR", [("0", "New"), ("4", "Canceled"), ("8", "Rejected"), ("F", "Trade")], pages=(6,)),
        f(39, "OrdStatus", "CHAR", [("0", "New"), ("1", "PartiallyFilled"), ("2", "Filled"), ("4", "Canceled"), ("8", "Rejected")], pages=(6,)),
    ],
    "messages": [
        # Continuation of NewOrderSingle across the chunk boundary, plus a disagreement on OrderQty.
        {"msg_type": "D", "name": "NewOrderSingle", "direction": "to_venue", "pages": [5], "fields": [
            mf(38, "OrderQty", required=False), mf(40, "OrdType"), mf(44, "Price", required=False, when=[("OrdType", ["2"])]),
            mf(59, "TimeInForce", required=False), mf(9001, "Capacity"), mf(60, "TransactTime"),
        ]},
        {"msg_type": "8", "name": "ExecutionReport", "direction": "from_venue", "pages": [6], "fields": [
            mf(37, "OrderID"), mf(11, "ClOrdID"), mf(17, "ExecID"), mf(150, "ExecType"), mf(39, "OrdStatus"),
            mf(48, "SecurityID"), mf(54, "Side"), mf(151, "LeavesQty"), mf(14, "CumQty"), mf(6, "AvgPx"),
            mf(58, "Text", required=False),
        ]},
    ],
    "notes": [],
}


def fake_extractor(chunk, venue_name):
    return CHUNK_1 if chunk.first_page == 1 else CHUNK_2


def test_split_pdf_overlaps(tmp_path):
    chunks = split_pdf(blank_pdf(tmp_path / "s.pdf", 23), pages_per_chunk=10, overlap=2)
    assert [(c.first_page, c.last_page) for c in chunks] == [(1, 10), (9, 18), (17, 23)]
    assert all(len(pypdf.PdfReader(__import__("io").BytesIO(c.pdf)).pages) == c.last_page - c.first_page + 1 for c in chunks)


def test_pipeline_produces_a_spec_codegen_accepts(tmp_path):
    pdf = blank_pdf(tmp_path / "venue.pdf", 8)
    spec_path, report_path, ok = run(pdf, "test_venue", "Test Venue", tmp_path / "out",
                                     extractor=fake_extractor, pages_per_chunk=5, log=lambda *_: None)
    report = report_path.read_text()
    assert ok, report
    spec = yaml.safe_load(spec_path.read_text())
    assert not [i for i in validate(spec) if i.level == "error"]

    nos = next(m for m in spec["messages"] if m["msg_type"] == "D")
    names = [x["field"] for x in nos["fields"]]
    assert names[:6] == ["ClOrdID", "HandlInst", "SecurityID", "SecurityIDSource", "Side", "OrderQty"]
    assert "Capacity" in names and "MsgSeqNum" not in names
    assert next(x for x in nos["fields"] if x["field"] == "OrderQty")["required"] is True  # conflict -> required
    assert next(x for x in nos["fields"] if x["field"] == "Price")["required_when"] == {"OrdType": ["2"]}
    assert not any(m["msg_type"] == "A" for m in spec["messages"])
    assert {x["field"] for x in spec["session"]["logon_fields"]} == {"Username", "Password"}

    oe = spec["order_entry"]
    assert oe["instrument"] == {"field": "SecurityID", "extra": {"SecurityIDSource": "4"}}
    assert oe["side"] == {"buy": "1", "sell": "2", "sell_short": "5"}  # merged across chunks
    assert oe["time_in_force"] == {"day": "0", "ioc": "3"}
    assert oe["defaults"] == {"HandlInst": "1"}
    assert oe["cancel"] is False and oe["replace"] is False

    assert "Price as optional" in report                      # model's note surfaced
    assert "9=VenueSpecialAuction" in report                   # unmapped enum surfaced
    assert "OrderQty required=True vs False" in report         # conflict surfaced
    assert "TransactTime(60) used in NewOrderSingle" in report  # standard fallback surfaced

    # The output is directly usable by codegen (file name == venue id).
    files = generate([spec_path], tmp_path / "gen")
    assert any(p.name == "test_venue.cpp" for p in files)


def test_chunk_cache_allows_resume(tmp_path):
    pdf = blank_pdf(tmp_path / "venue.pdf", 8)
    run(pdf, "v", "V", tmp_path / "out", extractor=fake_extractor, pages_per_chunk=5, log=lambda *_: None)

    def boom(chunk, name):
        raise AssertionError("should have used the cache")
    _, _, ok = run(pdf, "v", "V", tmp_path / "out", extractor=boom, pages_per_chunk=5, log=lambda *_: None)
    assert ok


# ------------------------------------------------------------------ ClaudeExtractor wiring
class FakeStream:
    def __init__(self, msg):
        self.msg = msg

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def get_final_message(self):
        return self.msg


class FakeClient:
    def __init__(self, msg):
        self.calls = []
        self.beta = SimpleNamespace(messages=SimpleNamespace(stream=self._stream))
        self._msg = msg

    def _stream(self, **kw):
        self.calls.append(kw)
        return FakeStream(self._msg)


def chunk(tmp_path):
    return split_pdf(blank_pdf(tmp_path / "c.pdf", 2), pages_per_chunk=5)[0]


def test_claude_extractor_request_shape(tmp_path):
    pytest.importorskip("anthropic")
    msg = SimpleNamespace(stop_reason="end_turn", stop_details=None,
                          content=[SimpleNamespace(type="text", text=json.dumps(CHUNK_2))])
    client = FakeClient(msg)
    out = ClaudeExtractor(client=client)(chunk(tmp_path), "Test Venue")
    assert out == CHUNK_2
    kw = client.calls[0]
    assert kw["model"] == "claude-opus-5-5"
    assert kw["output_config"]["format"] == {"type": "json_schema", "schema": CHUNK_SCHEMA}
    assert kw["thinking"] == {"type": "adaptive"}
    assert kw["fallbacks"] == "default"
    doc = kw["messages"][0]["content"][0]
    assert doc["type"] == "document" and doc["source"]["media_type"] == "application/pdf"


@pytest.mark.parametrize("stop", ["refusal", "max_tokens"])
def test_claude_extractor_surfaces_bad_stops(tmp_path, stop):
    pytest.importorskip("anthropic")
    msg = SimpleNamespace(stop_reason=stop, stop_details=SimpleNamespace(category="cyber"), content=[])
    with pytest.raises(ExtractionError, match="declined" if stop == "refusal" else "max_tokens"):
        ClaudeExtractor(client=FakeClient(msg))(chunk(tmp_path), "V")


# ------------------------------------------------------------------ OpenAI-compatible (free) backend
def test_openai_compatible_extractor_request_and_validation(tmp_path):
    from fixgen.specgen.openai_compat import OpenAICompatibleExtractor

    calls = []

    def post(url, body):
        calls.append((url, body))
        return {"choices": [{"finish_reason": "stop", "message": {"content": "```json\n" + json.dumps(CHUNK_2) + "\n```"}}]}

    ex = OpenAICompatibleExtractor(model="qwen2.5:14b", base_url="ollama", post=post)
    assert ex(chunk(tmp_path), "V") == CHUNK_2
    url, body = calls[0]
    assert url == "http://localhost:11434/v1/chat/completions"
    assert body["model"] == "qwen2.5:14b" and body["temperature"] == 0
    assert body["response_format"]["json_schema"]["schema"] == CHUNK_SCHEMA
    assert "===== page 1 =====" in body["messages"][1]["content"]


def test_openai_compatible_extractor_repairs_once_then_fails(tmp_path):
    from fixgen.specgen.openai_compat import OpenAICompatibleExtractor

    replies = iter(['{"fields": []}', json.dumps(CHUNK_2)])
    calls = []

    def post(url, body):
        calls.append(body)
        return {"choices": [{"finish_reason": "stop", "message": {"content": next(replies)}}]}

    ex = OpenAICompatibleExtractor(model="m", base_url="http://x/v1", structured="json_object", post=post)
    assert ex(chunk(tmp_path), "V") == CHUNK_2
    assert len(calls) == 2 and "invalid" in calls[1]["messages"][-1]["content"]
    assert calls[0]["response_format"] == {"type": "json_object"}

    bad = OpenAICompatibleExtractor(model="m", base_url="http://x/v1",
                                    post=lambda u, b: {"choices": [{"message": {"content": "nope"}}]})
    with pytest.raises(ExtractionError, match="did not match"):
        bad(chunk(tmp_path), "V")


def test_openai_compatible_extractor_backs_off_on_rate_limit(tmp_path):
    import io as _io
    import urllib.error

    from fixgen.specgen.openai_compat import OpenAICompatibleExtractor

    state = {"n": 0}
    slept = []

    def post(url, body):
        state["n"] += 1
        if state["n"] < 3:
            raise urllib.error.HTTPError(url, 429, "Too Many Requests", {"Retry-After": "7"}, _io.BytesIO(b"slow down"))
        return {"choices": [{"message": {"content": json.dumps(CHUNK_2)}}]}

    ex = OpenAICompatibleExtractor(model="m", base_url="gemini", post=post, sleep=slept.append)
    assert ex(chunk(tmp_path), "V") == CHUNK_2
    assert slept == [7.0, 7.0]
