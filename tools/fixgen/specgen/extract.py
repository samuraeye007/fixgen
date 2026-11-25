"""PDF -> venue spec pipeline.

  1. split   the PDF into overlapping page chunks (pypdf)
  2. extract each chunk with Claude, constrained to CHUNK_SCHEMA (structured outputs)
  3. merge   chunk results deterministically, recording every conflict
  4. derive  the order_entry mapping from the merged enums (standard FIX values only)
  5. check   with the same schema + lint used by codegen, and write a review report

Only step 2 uses the model. Everything after it is plain code, so a reviewer can see
exactly why each line of the output spec is there.
"""
from __future__ import annotations

import base64
import io
import json
import re
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Protocol

import yaml

from ..fix_standard import SESSION_MSG_TYPES, SESSION_TAGS, STANDARD_ENUMS, STANDARD_FIELDS
from ..spec import validate
from .prompts import SYSTEM, chunk_prompt
from .schema import CHUNK_SCHEMA

DEFAULT_MODEL = "claude-opus-5-5"


# ----------------------------------------------------------------------------- split
@dataclass
class Chunk:
    first_page: int  # 1-based, inclusive
    last_page: int
    total_pages: int
    pdf: bytes


def split_pdf(path: str | Path, pages_per_chunk: int = 25, overlap: int = 2) -> list[Chunk]:
    """Overlap keeps tables that straddle a chunk boundary whole in at least one chunk."""
    from pypdf import PdfReader, PdfWriter

    reader = PdfReader(str(path))
    total = len(reader.pages)
    step = max(1, pages_per_chunk - overlap)
    chunks = []
    start = 0
    while start < total:
        end = min(total, start + pages_per_chunk)
        writer = PdfWriter()
        for i in range(start, end):
            writer.add_page(reader.pages[i])
        buf = io.BytesIO()
        writer.write(buf)
        chunks.append(Chunk(start + 1, end, total, buf.getvalue()))
        if end == total:
            break
        start += step
    return chunks


# ----------------------------------------------------------------------------- extract
class Extractor(Protocol):
    def __call__(self, chunk: Chunk, venue_name: str) -> dict: ...


class ExtractionError(RuntimeError):
    pass


class ClaudeExtractor:
    """One structured-output request per chunk, with the chunk attached as a PDF document."""

    def __init__(self, model: str = DEFAULT_MODEL, effort: str = "high", client=None):
        import anthropic

        self.client = client or anthropic.Anthropic()
        self.model = model
        self.effort = effort

    def __call__(self, chunk: Chunk, venue_name: str) -> dict:
        import anthropic

        content = [
            {"type": "document",
             "source": {"type": "base64", "media_type": "application/pdf",
                        "data": base64.standard_b64encode(chunk.pdf).decode()}},
            {"type": "text", "text": chunk_prompt(venue_name, chunk.first_page, chunk.last_page, chunk.total_pages)},
        ]
        try:
            # Streaming: long PDFs and large structured outputs can exceed non-streaming timeouts.
            with self.client.beta.messages.stream(
                model=self.model,
                max_tokens=64000,
                betas=["server-side-fallback-2026-07-01"],
                fallbacks="default",  # retry on a fallback model if this one declines
                thinking={"type": "adaptive"},
                output_config={"effort": self.effort,
                               "format": {"type": "json_schema", "schema": CHUNK_SCHEMA}},
                system=SYSTEM,
                messages=[{"role": "user", "content": content}],
            ) as stream:
                msg = stream.get_final_message()
        except anthropic.APIStatusError as e:
            raise ExtractionError(f"pages {chunk.first_page}-{chunk.last_page}: API error {e.status_code}: {e.message}") from e
        except anthropic.APIConnectionError as e:
            raise ExtractionError(f"pages {chunk.first_page}-{chunk.last_page}: connection error: {e}") from e

        where = f"pages {chunk.first_page}-{chunk.last_page}"
        if msg.stop_reason == "refusal":
            cat = getattr(msg.stop_details, "category", None) if msg.stop_details else None
            raise ExtractionError(f"{where}: model declined (category={cat})")
        if msg.stop_reason == "max_tokens":
            raise ExtractionError(f"{where}: output hit max_tokens; use a smaller --pages-per-chunk")
        text = "".join(b.text for b in msg.content if b.type == "text")
        try:
            return json.loads(text)
        except json.JSONDecodeError as e:
            raise ExtractionError(f"{where}: model output was not valid JSON: {e}") from e


# ----------------------------------------------------------------------------- merge
_IDENT = re.compile(r"[^A-Za-z0-9_]")


def ident(name: str, fallback: str) -> str:
    """Document names -> FIX-style identifiers: "New Order Single" -> "NewOrderSingle"."""
    words = re.split(r"[\s\-/]+", name.strip())
    if len(words) > 1:
        name = "".join(w[:1].upper() + w[1:] for w in words)
    s = _IDENT.sub("", name)
    if not s or not s[0].isalpha():
        s = fallback + s
    return s


@dataclass
class Review:
    conflicts: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    assumptions: list[str] = field(default_factory=list)
    dropped: list[str] = field(default_factory=list)


def merge(results: list[tuple[Chunk, dict]], review: Review) -> dict:
    fields: dict[int, dict] = {}
    messages: dict[str, dict] = {}
    session: dict = {"logon_fields": {}}

    for chunk, r in results:
        where = f"pp.{chunk.first_page}-{chunk.last_page}"
        review.notes.extend(f"({where}) {n}" for n in r.get("notes", []))

        s = r.get("session", {})
        for key in ("begin_string", "default_appl_ver_id", "heartbeat_interval", "reset_seq_num_on_logon"):
            v = s.get(key)
            if v is None:
                continue
            if key in session and session[key] != v:
                review.conflicts.append(f"session.{key}: {session[key]!r} vs {v!r} ({where}); kept the first")
            else:
                session[key] = v
        for lf in s.get("logon_fields", []):
            session["logon_fields"].setdefault(lf["tag"], lf)

        for f in r.get("fields", []):
            tag = f["tag"]
            if tag in SESSION_TAGS and tag not in (553, 554, 925):
                continue
            f = {**f, "name": ident(f["name"], "Tag")}
            cur = fields.get(tag)
            if cur is None:
                fields[tag] = {**f, "values": list(f.get("values", [])), "pages": list(f.get("pages", []))}
                continue
            if cur["name"] != f["name"]:
                review.conflicts.append(f"tag {tag} named {cur['name']} and {f['name']} ({where}); kept {cur['name']}")
            if cur["type"] != f["type"]:
                if cur["type"] == "STRING":
                    cur["type"] = f["type"]
                elif f["type"] != "STRING":
                    review.conflicts.append(f"tag {tag} type {cur['type']} vs {f['type']} ({where}); kept {cur['type']}")
            have = {v["value"]: v for v in cur["values"]}
            for v in f.get("values", []):
                if v["value"] not in have:
                    cur["values"].append(v)
                elif have[v["value"]]["name"] != v["name"]:
                    review.conflicts.append(f"{cur['name']}={v['value']} named {have[v['value']]['name']} and {v['name']}")
            cur["custom"] = cur.get("custom") or f.get("custom")
            cur["description"] = cur.get("description") or f.get("description", "")
            cur["pages"] = sorted(set(cur["pages"]) | set(f.get("pages", [])))

        for m in r.get("messages", []):
            mt = m["msg_type"].strip()
            if mt in SESSION_MSG_TYPES:
                continue
            cur = messages.get(mt)
            if cur is None:
                messages[mt] = {**m, "msg_type": mt, "fields": list(m["fields"]), "pages": list(m.get("pages", []))}
                continue
            # Same message spread over a chunk boundary (or repeated): union, keep order.
            have = {mf["tag"]: mf for mf in cur["fields"]}
            for mf in m["fields"]:
                if mf["tag"] not in have:
                    cur["fields"].append(mf)
                elif have[mf["tag"]]["required"] != mf["required"]:
                    review.conflicts.append(f"{cur['name']}.{mf['field']} required={have[mf['tag']]['required']} vs "
                                            f"{mf['required']} ({where}); using required=true")
                    have[mf["tag"]]["required"] = True
            cur["pages"] = sorted(set(cur["pages"]) | set(m.get("pages", [])))

    return {"fields": fields, "messages": messages, "session": session}


# ----------------------------------------------------------------------------- build spec
def _enum_by_value(f: dict | None) -> dict[str, str]:
    return {v["value"]: v["name"] for v in (f or {}).get("values", [])}


def derive_order_entry(fields_by_name: dict[str, dict], msgs: dict[str, dict], review: Review) -> dict:
    oe: dict = {}
    nos = msgs.get("D", {"fields": []})
    nos_fields = {mf["field"]: mf for mf in nos["fields"]}

    if "Symbol" in nos_fields and nos_fields["Symbol"].get("required"):
        oe["instrument"] = {"field": "Symbol"}
    elif "SecurityID" in nos_fields:
        inst = {"field": "SecurityID"}
        src = _enum_by_value(fields_by_name.get("SecurityIDSource"))
        if "SecurityIDSource" in nos_fields and len(src) == 1:
            inst["extra"] = {"SecurityIDSource": next(iter(src))}
            review.assumptions.append(f"instrument is SecurityID with SecurityIDSource={next(iter(src))} "
                                      f"({next(iter(src.values()))}), the only value the document lists")
        elif "SecurityIDSource" in nos_fields:
            review.assumptions.append("SecurityIDSource has several values; clients must send it as an extra. "
                                      "Set order_entry.instrument.extra if one should be fixed.")
        oe["instrument"] = inst
    else:
        oe["instrument"] = {"field": "Symbol"}
        review.assumptions.append("no Symbol/SecurityID in NewOrderSingle; defaulted instrument to Symbol: CHECK")

    if "Account" in nos_fields:
        oe["account"] = {"field": "Account", "required": bool(nos_fields["Account"].get("required"))}

    for key, (fname, std) in STANDARD_ENUMS.items():
        venue_vals = _enum_by_value(fields_by_name.get(fname))
        if not venue_vals:
            if fname in fields_by_name:
                review.assumptions.append(f"{fname} has no enumerated values in the document; "
                                          f"assumed standard FIX values for order_entry.{key}")
                oe[key] = {n: v for n, v in std.items() if key != "exec_type" or n not in ("partial_fill", "fill")}
            continue
        mapping = {norm: v for norm, v in std.items() if v in venue_vals}
        unmapped = [f"{v}={n}" for v, n in venue_vals.items() if v not in mapping.values()]
        if unmapped:
            review.assumptions.append(f"{fname} values not mapped to the normalized model: {', '.join(unmapped)}")
        if mapping:
            oe[key] = mapping

    defaults = {}
    for mf in nos["fields"]:
        f = fields_by_name.get(mf["field"])
        if mf.get("required") and f and len(f.get("values", [])) == 1 and mf["field"] not in (
                "Side", "OrdType", "TimeInForce", "SecurityIDSource"):
            defaults[mf["field"]] = f["values"][0]["value"]
            review.assumptions.append(f"{mf['field']} is required with a single allowed value; "
                                      f"defaulted to {f['values'][0]['value']}")
    oe["defaults"] = defaults
    oe["cancel"] = "F" in msgs
    oe["replace"] = "G" in msgs
    for key in ("side", "ord_type", "exec_type", "ord_status"):
        oe.setdefault(key, {})
    return oe


def build_spec(merged: dict, venue_id: str, venue_name: str, source: str, review: Review) -> dict:
    fields = merged["fields"]
    # Fill in standard fields that messages reference but the document never defines.
    for m in merged["messages"].values():
        m["name"] = ident(m["name"], "Msg")
        for mf in m["fields"]:
            tag = mf["tag"]
            if tag not in fields and tag in STANDARD_FIELDS:
                name, typ = STANDARD_FIELDS[tag]
                fields[tag] = {"tag": tag, "name": name, "type": typ, "custom": False, "values": [], "pages": []}
                review.assumptions.append(f"{name}({tag}) used in {m['name']} but not defined; took the standard definition")
    for lf in merged["session"]["logon_fields"].values():
        if lf["tag"] not in fields:
            name, typ = STANDARD_FIELDS.get(lf["tag"], (ident(lf["field"], "Tag"), "STRING"))
            fields[lf["tag"]] = {"tag": lf["tag"], "name": name, "type": typ, "custom": lf["tag"] >= 5000,
                                 "values": [], "pages": lf.get("pages", [])}

    by_tag = {t: f for t, f in fields.items()}
    by_name = {f["name"]: f for f in fields.values()}

    out_fields = []
    for tag in sorted(fields):
        f = fields[tag]
        of = {"tag": tag, "name": f["name"], "type": f["type"]}
        if f.get("custom"):
            of["custom"] = True
        if f.get("description"):
            of["description"] = f["description"]
        vals, seen = [], set()
        for v in f.get("values", []):
            name = ident(v["name"], "V")
            while name in seen:
                name += "_"
            seen.add(name)
            ov = {"value": v["value"], "name": name}
            if v.get("description"):
                ov["description"] = v["description"]
            vals.append(ov)
        if vals:
            of["values"] = vals
        if f.get("pages"):
            of["source"] = {"pages": f["pages"]}
        out_fields.append(of)

    out_msgs = []
    for mt, m in sorted(merged["messages"].items()):
        mfields, names = [], set()
        for mf in m["fields"]:
            if mf["tag"] in SESSION_TAGS:
                continue
            f = by_tag.get(mf["tag"])
            if f is None:
                review.dropped.append(f"{m['name']}: tag {mf['tag']} ({mf['field']}) is not defined anywhere")
                continue
            if f["name"] in names:
                continue
            names.add(f["name"])
            entry = {"field": f["name"], "required": bool(mf["required"])}
            conds = {}
            for c in mf.get("required_when", []):
                cname = ident(c["field"], "Tag")
                if cname in by_name:
                    conds[cname] = c["values"]
                else:
                    review.dropped.append(f"{m['name']}.{f['name']}: condition on unknown field {c['field']}")
            if conds:
                entry["required_when"] = conds
            if mf.get("note"):
                entry["note"] = mf["note"]
            mfields.append(entry)
        # A condition may name a field that is not in the message; lint would reject that.
        for entry in mfields:
            for cname in list(entry.get("required_when", {})):
                if cname not in names:
                    review.dropped.append(f"{m['name']}.{entry['field']}: condition on {cname}, which is not in the message")
                    del entry["required_when"][cname]
            if "required_when" in entry and not entry["required_when"]:
                del entry["required_when"]
        om = {"msg_type": mt, "name": ident(m["name"], "Msg"), "direction": m["direction"], "fields": mfields}
        if m.get("pages"):
            om["source"] = {"pages": m["pages"]}
        out_msgs.append(om)

    s = merged["session"]
    session = {"begin_string": s.get("begin_string") or "FIX.4.4"}
    if not s.get("begin_string"):
        review.assumptions.append("BeginString not found; assumed FIX.4.4: CHECK")
    if s.get("default_appl_ver_id"):
        session["default_appl_ver_id"] = s["default_appl_ver_id"]
    elif session["begin_string"] == "FIXT.1.1":
        session["default_appl_ver_id"] = "9"
        review.assumptions.append("FIXT.1.1 without DefaultApplVerID; assumed 9 (FIX 5.0 SP2): CHECK")
    if s.get("heartbeat_interval"):
        session["heartbeat_interval"] = s["heartbeat_interval"]
    if s.get("reset_seq_num_on_logon") is not None:
        session["reset_seq_num_on_logon"] = s["reset_seq_num_on_logon"]
    logon = []
    for tag, lf in sorted(s["logon_fields"].items()):
        entry = {"field": by_tag[tag]["name"]}
        if lf.get("credential"):
            entry["from"] = "credential." + re.sub(r"[^a-z_]", "", lf["credential"].lower().replace(" ", "_"))
        elif lf.get("value"):
            entry["value"] = lf["value"]
        else:
            entry["from"] = "credential." + re.sub(r"(?<!^)(?=[A-Z])", "_", entry["field"]).lower()
            review.assumptions.append(f"logon field {entry['field']} has no value; treated as credential {entry['from']}")
        logon.append(entry)
    if logon:
        session["logon_fields"] = logon

    return {
        "spec_version": 1,
        "venue": {"id": venue_id, "name": venue_name,
                  "description": f"Generated by fixgen specgen from {source}. Review before use.",
                  "source": {"document": source}},
        "session": session,
        "fields": out_fields,
        "messages": out_msgs,
        "order_entry": derive_order_entry(by_name, merged["messages"], review),
    }


# ----------------------------------------------------------------------------- report
def render_review(spec: dict, review: Review, issues) -> str:
    lines = [f"# specgen review: {spec['venue']['name']} (`{spec['venue']['id']}`)", "",
             f"Source: {spec['venue']['source']['document']}", "",
             f"{len(spec['fields'])} fields, {len(spec['messages'])} messages: "
             + ", ".join(f"{m['name']} (35={m['msg_type']})" for m in spec["messages"]), "",
             "Every item in the spec carries `source.pages`. Check the sections below first, then "
             "spot-check required flags and enum values against the PDF before committing.", ""]

    def section(title: str, items: list[str]) -> None:
        lines.append(f"## {title} ({len(items)})")
        lines.append("")
        lines.extend(f"- {i}" for i in items) if items else lines.append("None.")
        lines.append("")

    section("Validation errors (codegen will refuse this spec)", [str(i) for i in issues if i.level == "error"])
    section("Validation warnings", [str(i) for i in issues if i.level == "warning"])
    section("Conflicts between pages", review.conflicts)
    section("Assumptions made by specgen", review.assumptions)
    section("Dropped items", review.dropped)
    section("Ambiguities the model flagged", review.notes)
    lines += ["## Order entry mapping", "", "```yaml", yaml.safe_dump(spec["order_entry"], sort_keys=False).rstrip(), "```", ""]
    return "\n".join(lines)


# ----------------------------------------------------------------------------- driver
def run(pdf: str | Path, venue_id: str, venue_name: str, out_dir: str | Path,
        extractor: Extractor | None = None, pages_per_chunk: int = 25, workers: int = 4,
        log: Callable[[str], None] = print) -> tuple[Path, Path, bool]:
    """Returns (spec_path, report_path, ok). ok is False if the spec has validation errors."""
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    extractor = extractor or ClaudeExtractor()
    chunks = split_pdf(pdf, pages_per_chunk)
    log(f"{Path(pdf).name}: {chunks[0].total_pages if chunks else 0} pages in {len(chunks)} chunks")

    raw_dir = out / f"{venue_id}.chunks"
    raw_dir.mkdir(exist_ok=True)

    def one(c: Chunk) -> tuple[Chunk, dict]:
        cache = raw_dir / f"pp{c.first_page:04d}-{c.last_page:04d}.json"
        if cache.exists():  # resume: chunks already paid for are not re-extracted
            log(f"  pages {c.first_page}-{c.last_page}: cached")
            return c, json.loads(cache.read_text())
        log(f"  pages {c.first_page}-{c.last_page}: extracting")
        r = extractor(c, venue_name)
        cache.write_text(json.dumps(r, indent=1))
        return c, r

    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(one, chunks))

    review = Review()
    spec = build_spec(merge(results, review), venue_id, venue_name, Path(pdf).name, review)
    issues = validate(spec)
    spec_path = out / f"{venue_id}.yaml"
    report_path = out / f"{venue_id}.review.md"
    header = (f"# Generated by `fixgen specgen` from {Path(pdf).name}.\n"
              f"# Review {report_path.name} before copying this file into spec/venues/.\n")
    spec_path.write_text(header + yaml.safe_dump(spec, sort_keys=False, width=110, allow_unicode=True))
    report_path.write_text(render_review(spec, review, issues))
    ok = not any(i.level == "error" for i in issues)
    return spec_path, report_path, ok
