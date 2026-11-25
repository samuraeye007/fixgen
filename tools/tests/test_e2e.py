"""End to end: gateway + two fixgen-sim acceptors over real TCP, driven via the HTTP API.
Skipped unless the C++ build exists (cmake -S . -B build && cmake --build build)."""
import json
import socket
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
pytestmark = pytest.mark.skipif(not (BUILD / "fixgen-gateway").exists(), reason="C++ build not found")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def call(base, path, body=None):
    req = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="GET" if body is None else "POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def wait_for(pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError("timed out")


@pytest.fixture
def stack(tmp_path):
    demo_port, acme_port, http_port = free_port(), free_port(), free_port()
    procs = [
        subprocess.Popen([BUILD / "fixgen-sim", "--venue", "demo_exch", "--port", str(demo_port), "--fill", "partial", "--quiet"]),
        subprocess.Popen([BUILD / "fixgen-sim", "--venue", "acme_mtf", "--port", str(acme_port), "--fill", "immediate", "--quiet"]),
    ]
    cfg = json.loads((ROOT / "config" / "gateway.json").read_text())
    cfg["http"] = {"host": "127.0.0.1", "port": http_port, "ui_dir": str(ROOT / "ui")}
    cfg["sessions"][0]["port"] = demo_port
    cfg["sessions"][1]["port"] = acme_port
    (tmp_path / "gw.json").write_text(json.dumps(cfg))
    time.sleep(0.2)
    procs.append(subprocess.Popen([BUILD / "fixgen-gateway", "--config", tmp_path / "gw.json"],
                                  env={"DEMO_PASSWORD": "x", "ACME_PASSWORD": "y"}, stdout=subprocess.DEVNULL))
    base = f"http://127.0.0.1:{http_port}"
    try:
        wait_for(lambda: _active(base))
        yield base
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.wait(timeout=5)


def _active(base):
    try:
        status, sessions = call(base, "/api/sessions")
    except (urllib.error.URLError, ConnectionError):
        return False
    return status == 200 and all(s["state"] == "active" for s in sessions)


def orders(base):
    return {o["key"]: o for o in call(base, "/api/orders")[1]}


def test_order_lifecycle_across_two_venues(stack):
    base = stack
    st, r = call(base, "/api/orders", {"session": "demo", "symbol": "VOD", "side": "buy", "qty": "100",
                                       "ord_type": "limit", "price": "101.5", "tif": "day"})
    assert st == 201, r
    demo_key = r["orders"][0]
    # partial fill policy: 50 filled, 50 resting
    o = wait_for(lambda: (x := orders(base)[demo_key])["status"] == "partially_filled" and x)
    assert o["cum_qty"] == "50" and o["leaves_qty"] == "50"

    st, r = call(base, f"/api/orders/{demo_key}/replace", {"qty": "80", "price": "101.6"})
    assert st == 200, r
    o = wait_for(lambda: (x := orders(base)[demo_key])["pending"] == "" and x["qty"] == "80" and x)
    assert o["price"] == "101.6" and o["cl_ord_id"] != demo_key

    st, r = call(base, f"/api/orders/{demo_key}/cancel", {})
    assert st == 200, r
    wait_for(lambda: orders(base)[demo_key]["status"] == "canceled")

    # acme: different instrument model, mandatory custom tag, no market orders
    st, r = call(base, "/api/orders", {"session": "acme", "symbol": "GB00BH4HKS39", "side": "sell", "qty": "10",
                                       "ord_type": "market"})
    assert st == 400 and "market" in r["error"]
    st, r = call(base, "/api/orders", {"session": "acme", "symbol": "GB00BH4HKS39", "side": "sell", "qty": "10",
                                       "ord_type": "limit", "price": "2.5", "account": "A1"})
    assert st == 400 and "ClientCapacity" in r["error"]
    st, r = call(base, "/api/orders", {"session": "acme", "symbol": "GB00BH4HKS39", "side": "sell", "qty": "10",
                                       "ord_type": "limit", "price": "2.5", "account": "A1",
                                       "extra": {"ClientCapacity": "P"}, "count": 25})
    assert st == 201 and len(r["orders"]) == 25
    wait_for(lambda: sum(o["status"] == "filled" for o in orders(base).values()) == 25)


def test_sim_rejects_and_gateway_reports(stack):
    base = stack
    st, r = call(base, "/api/orders", {"session": "demo", "symbol": "REJECT", "side": "buy", "qty": "1",
                                       "ord_type": "market"})
    assert st == 201
    key = r["orders"][0]
    o = wait_for(lambda: (x := orders(base)[key])["status"] == "rejected" and x)
    assert "not tradeable" in o["text"]


def test_rate_limited_blast(stack):
    base = stack
    st, r = call(base, "/api/orders", {"session": "acme", "symbol": "GB00BH4HKS39", "side": "buy", "qty": "1",
                                       "ord_type": "limit", "price": "1", "account": "A1",
                                       "extra": {"ClientCapacity": "A"}, "count": 2000, "rate_per_sec": "4000"})
    assert st == 201 and len(r["queued"]) == 2000
    wait_for(lambda: sum(o["status"] == "filled" for o in orders(base).values()) == 2000, timeout=20)
