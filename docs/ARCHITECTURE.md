# Architecture

## The idea

Most of what makes venues different on the wire is data, not logic:

- which tags each message carries
- which of those are mandatory, and under what conditions
- which enum values the venue accepts
- how the instrument is identified
- which custom tags it requires
- what goes on the Logon

fixgen captures that data in one standardised file per venue and generates C++ tables
from it. A single, well-tested engine (`fixcore::SpecAdapter`) interprets those tables.
The LLM's only job is to read the PDF and produce a first draft of that file. A person
reviews the draft before anything gets compiled.

## Components

### 1. Venue spec: `spec/venues/<id>.yaml`

The schema is in `spec/schema/venue-spec.schema.json`, and `tools/fixgen/spec.py` holds
the semantic lint. A spec has these sections:

- **session**: BeginString (FIX.4.2 to 4.4, or FIXT.1.1 with DefaultApplVerID),
  heartbeat interval, reset-on-logon, and extra Logon fields. Each extra field is either a
  constant or `from: credential.<key>`, so secrets never live in the spec.
- **fields**: tag, name, type, enum values, and a custom flag. Optionally
  `source.pages` pointing back into the PDF.
- **messages**: the body fields of each application message, with `required` and
  `required_when` (for example "Price is required when OrdType=2").
- **order_entry**: maps the core's normalized model (buy/sell, limit/market, day/ioc,
  trade/canceled, and so on) onto this venue's wire values. It also sets the instrument
  field (`Symbol`, or `SecurityID` plus a fixed `SecurityIDSource`), the account field,
  default values, and whether cancel and replace are supported.

Lint enforces cross-references that the JSON Schema cannot express. It rejects:

- undefined fields
- duplicate tags
- mappings to values that are not in the field's enum
- conditions that name fields the message does not contain
- replace enabled with no 35=G message
- FIXT without an ApplVerID

Codegen refuses any spec with lint errors.

**Why not use FIX Orchestra directly?** Orchestra is the right interchange standard, and
an importer and exporter for it belong on the roadmap. As the target for LLM output and
as the source for codegen, a small purpose-built schema works better, for two reasons:

- Constrained decoding is more reliable against a compact schema than against
  Orchestra's XML repository model.
- The part that matters most for an order-routing core is the mapping from *our* order
  model to *their* values, and Orchestra has no slot for that.

The plan is to import Orchestra (or QuickFIX XML) when a venue publishes it, because a
published dictionary is authoritative and needs no LLM, and to export Orchestra for
anyone downstream.

### 2. Codegen: `tools/fixgen/codegen.py`

For each spec, codegen emits two files:

- `fixgen/venues/<id>.hpp`: typed constants (`tag::ClientCapacity`,
  `values::Side::Buy`) for any hand-written code.
- `<id>.cpp`: constexpr tables for the `Dictionary` (fields, enums, message rules) and
  the `VenueProfile` (mappings).

It also emits `registry.cpp`, which defines `make_all_venues()`. CMake runs codegen
whenever a spec, a template or the schema changes.

The generated code is deliberately data, not logic. Behaviour lives in `SpecAdapter` and
`Dictionary::validate`, which are tested once rather than once per venue. When a venue
has a quirk the spec cannot express, subclass `SpecAdapter`, override the hook, and
register the subclass. Don't add a special case to the templates.

### 3. Core: `core/`

| File | Contents |
|---|---|
| `message`, `codec` | Ordered tag=value message (repeating groups preserved by order), encoding with BodyLength and CheckSum, decoding, and a stream `FrameParser` |
| `session` | FIX session state machine for initiator and acceptor. Covers logon and logout, heartbeat and TestRequest, sequence checks, gap detection with ResendRequest, resend with PossDupFlag, and gap fill of admin messages. It is transport-agnostic: feed it bytes and it calls back with bytes |
| `dictionary` | Validates a message against the venue's rules: required and conditional fields, enum values, type syntax, and fields the venue does not accept |
| `spec_adapter` | Normalized `NewOrder` / `CancelOrder` / `ReplaceOrder` → venue FIX, ExecutionReport / OrderCancelReject → `ExecEvent`. It returns a precise error when the venue cannot do what was asked (for example "ACME MTF does not support market orders") |
| `net`, `json` | A poll() reactor, non-blocking TCP and a small JSON library. No third-party dependencies |

Every outbound message is validated against the venue dictionary before it is sent, and
every inbound execution is validated too. When the venue deviates from its own spec, the
deviation shows up as a warning on the order in the UI.

### 4. Gateway and Order Blaster: `gateway/`, `ui/`

`fixgen-gateway` runs one initiator session per configured venue and reconnects
automatically. It tracks orders across cancel/replace chains, keyed by the first
ClOrdID. It also serves the UI and a JSON/SSE API.

Blasting sends up to 1,000 orders inline. Larger blasts, or any blast with
`rate_per_sec`, go through a token-bucket queue.

The UI has no build step. It builds the ticket from each session's capabilities, so
side, order-type and TIF choices, the account requirement and venue-specific extra
fields all come from the spec. It shows a live blotter with cancel and amend, plus a
filterable FIX log with credentials redacted.

## Do you need an exchange simulator?

Not a real one. What you need to test splits into three layers:

| Layer | What tests it | Built? |
|---|---|---|
| Codec, session, translation logic | Unit tests (`fixcore_tests`), including gap-fill, resend and heartbeat-timeout scenarios | Yes |
| The whole system over sockets: UI/API → gateway → FIX → back | `fixgen-sim`, a thin acceptor that reuses the core's session layer and the venue's own generated dictionary. Acks, fills (immediate or partial), cancels, replaces, rejects | Yes |
| "Our spec matches the real venue" | The venue's UAT / certification environment, then golden-message tests from the PDF's sample messages and recorded UAT sessions | Process, not code |

The sim cost almost nothing to build because it is the core running as an acceptor. Keep
its limit in mind: it shares the spec with the gateway, so it **cannot catch a wrong
spec**. If the spec says Price is optional and the real venue requires it, both sides
agree with each other and the test passes.

Only the venue's test environment catches that class of error. Certification is
mandatory before production at essentially every venue anyway. A matching engine,
market data or realistic fill behaviour would add a lot of code without closing that
gap. Write one only if you need deterministic load or latency testing.

Cheap ways to strengthen the middle layer later:

- **Golden messages.** Extract the sample messages printed in each PDF (specgen can do
  this as a second pass). Assert that `Dictionary::validate` accepts them and that the
  adapter produces equivalent ones.
- **Replay.** Record UAT sessions and replay the venue side through the sim, so
  regression tests use real venue behaviour.
- **Independent counterparty.** Point the gateway at QuickFIX/J's `Executor` example with
  the venue's dictionary. The two implementations share no code, which catches
  session-layer bugs that a self-consistent pair would miss.

## Specgen: `tools/fixgen/specgen/`

The pipeline has five steps. Only step 2 uses the model.

1. **Split** the PDF into 25-page chunks that overlap by 2 pages, so a table that
   straddles a boundary is whole in at least one chunk.
2. **Extract.** Each chunk goes to Claude (`claude-opus-5-5`, adaptive thinking, effort
   `high`) as a PDF document. Structured outputs constrain the response to
   `CHUNK_SCHEMA`. The prompt says to extract only what the pages state, never to fill
   in "standard FIX", and to report ambiguities in `notes` with page numbers. Refusals
   and truncation raise errors instead of producing partial specs. Server-side fallback
   is enabled (`fallbacks: "default"`), so if the model declines, the request retries on
   a fallback model. Chunks run in parallel, and results are cached per chunk so a
   re-run resumes where it stopped.
3. **Merge** deterministically:
   - Fields are keyed by tag, and messages by MsgType. A message split across chunks is
     unioned in document order.
   - Every disagreement is recorded: names, types, enum names, and required flags.
     Conflicting required flags resolve to required.
   - Header, trailer and session messages are dropped.
4. **Derive `order_entry` by code, not by the model.** Only standard FIX values are
   mapped (for example Side `1`→buy). Venue-specific values are listed as unmapped for a
   human to decide. Single-valued required fields become defaults. The instrument field
   comes from what NewOrderSingle requires.
5. **Check and report.** The same schema and lint that codegen uses run on the result,
   and `<id>.review.md` lists:
   - validation errors and warnings
   - conflicts between pages
   - every assumption specgen made
   - dropped items
   - the model's notes
   - the derived mapping

The tests (`tools/tests/test_specgen.py`) run the whole pipeline with a fake model and
check that its output compiles through codegen. They also check the shape of the real
API request.

**Cost and limits.** A 150-page spec is about 7 chunks. Each sends up to 25 PDF pages
(text plus page images) and gets back a few thousand output tokens. Expect a few dollars
per venue at Opus pricing. That is small next to the review time, which is the real cost
and should stay.

Repeating groups (Parties and similar) are not modelled yet. See the roadmap.

## Known gaps and roadmap

Roughly in priority order:

1. **Sequence-number persistence.** Sequence numbers live in memory only. That is fine
   for venues that reset on logon, but not for 24h sessions. Add a file-backed store
   and a resend store.
2. **TLS.** Many venues require TLS (or a VPN or leased line). Add OpenSSL to `net`, or
   front the connection with stunnel for now.
3. **Repeating groups in the spec** (Parties, NoPartyIDs), with group-aware validation
   and adapter support. Many MiFID II venues need these for short codes.
4. **Orchestra / QuickFIX XML import**, so a venue's published dictionary can skip
   specgen, and an Orchestra export.
5. **Golden-message tests** from PDF examples and recorded UAT sessions.
6. **Risk checks** in the gateway (max quantity and notional, fat-finger price bands,
   kill switch). They are needed before an order blaster goes anywhere near production.
7. **Persistence** of the order store, **auth** on the gateway API (it currently binds
   to localhost with no auth), and an audit log.
8. **Mass cancel** and order status requests. Market data is out of scope for now.
