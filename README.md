# fixgen

Multi-venue FIX order entry, built so that adding an exchange means reviewing a spec
file instead of writing an adapter by hand.

```
   exchange PDF ──► specgen (Claude) ──► spec/venues/<id>.yaml ──► codegen ──► C++ adapter tables
                                         (reviewed, committed)                     │
   Order Blaster UI ◄── HTTP/SSE ──► fixgen-gateway (core + adapters) ◄── FIX ──► venue / fixgen-sim
```

| Part | Where | What it is |
|---|---|---|
| Order Blaster | `ui/index.html` | Web UI. It builds the ticket from each venue's capabilities, then blasts, cancels, amends and shows a live blotter and FIX log |
| Gateway | `gateway/` | Holds the FIX sessions, translates normalized orders through the venue adapters, and serves the UI and a JSON/SSE API |
| Core | `core/` | C++20 FIX codec, session layer (logon, heartbeats, gap fill, resend), dictionary validation, and `SpecAdapter` |
| Venue specs | `spec/venues/*.yaml` | One standardised file per venue. Schema in `spec/schema/venue-spec.schema.json` |
| Codegen | `tools/fixgen/codegen.py` | Turns a spec into constexpr C++ tables plus typed tag constants. It runs as part of the CMake build |
| Specgen | `tools/fixgen/specgen/` | Turns a PDF into a draft spec and a review report, using Claude |
| Sim | `sim/` | A thin, spec-driven FIX acceptor for tests. It is not a matching engine |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and the reasons behind it.

## Quick start

Requirements: a C++20 compiler, CMake ≥ 3.20, Python ≥ 3.10.

```bash
pip install -e '.[specgen,test]'
```

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

```bash
./build/fixcore_tests && python -m pytest
```

```bash
./scripts/dev.sh
```

`scripts/dev.sh` starts one simulator per sample venue plus the gateway. Then open
http://127.0.0.1:8080.

## Adding a venue

1. Get a draft spec from the venue's PDF:

   ```bash
   export ANTHROPIC_API_KEY=...
   ```

   ```bash
   fixgen specgen path/to/venue-fix-spec.pdf --venue-id xyz --venue-name "XYZ Exchange"
   ```

   This writes `specgen-out/xyz.yaml` and `specgen-out/xyz.review.md`. Each chunk's raw
   extraction is cached in `specgen-out/xyz.chunks/`, so a re-run only pays for chunks
   that are missing.
2. Work through the review report: validation errors, conflicts between pages, every
   assumption specgen made, and the ambiguities the model flagged. Each field and message
   carries `source.pages`, so you can check it against the PDF.
3. Copy the reviewed file to `spec/venues/xyz.yaml`, then check it:

   ```bash
   fixgen validate spec/venues/xyz.yaml
   ```

4. Re-run CMake. The venue is now in `make_all_venues()`. Add a session for it to
   `config/gateway.json`.
5. Smoke test against `fixgen-sim --venue xyz`, then certify in the venue's UAT
   environment.

If the venue publishes a machine-readable dictionary (FIX Orchestra or QuickFIX XML),
prefer it over the PDF. See the roadmap in the architecture doc.

## Gateway API

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/sessions` | Sessions, their state, and each venue's capabilities (sides, order types, TIFs, extra fields) |
| POST | `/api/sessions/{name}/connect` and `.../disconnect` | |
| GET | `/api/orders` | Order records |
| POST | `/api/orders` | `{session, symbol, side, qty, ord_type, price?, tif?, account?, extra?, count?, rate_per_sec?}` |
| POST | `/api/orders/{key}/cancel` | |
| POST | `/api/orders/{key}/replace` | `{qty?, price?}` |
| GET | `/api/events` | Server-Sent Events: `session`, `order`, `fix` (passwords redacted), `notice` |

Prices and quantities are decimal strings throughout, so nothing is rounded through a
`double`.

Credentials in `config/gateway.json` can reference environment variables, for example
`"${DEMO_PASSWORD}"`.
