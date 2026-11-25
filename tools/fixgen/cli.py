"""fixgen command line.

  fixgen validate spec/venues/*.yaml
  fixgen codegen --out build/generated spec/venues/*.yaml
  fixgen specgen --venue-id xyz --model qwen2.5:14b spec.pdf               # local Ollama, free
  fixgen specgen --venue-id xyz --base-url gemini --model gemini-2.5-flash spec.pdf  # free tier
  fixgen specgen --venue-id xyz --provider claude spec.pdf
"""
from __future__ import annotations

import argparse
import sys

from .spec import SpecError, load_spec, validate


def cmd_validate(args) -> int:
    bad = 0
    for path in args.specs:
        issues = validate(load_spec(path))
        errors = [i for i in issues if i.level == "error"]
        bad += bool(errors)
        print(f"{path}: {'FAIL' if errors else 'ok'}")
        for i in issues:
            print(f"  {i}")
    return 1 if bad else 0


def cmd_codegen(args) -> int:
    from pathlib import Path

    from .codegen import generate

    for p in args.specs:  # file name is the venue id: keeps CMake's output list honest
        stem, vid = Path(p).stem, load_spec(p).get("venue", {}).get("id")
        if stem != vid:
            print(f"{p}: file name must match venue.id ({vid!r})", file=sys.stderr)
            return 1
    try:
        files = generate(args.specs, args.out)
    except SpecError as e:
        print(f"spec validation failed:\n{e}", file=sys.stderr)
        return 1
    if args.verbose:
        for f in files:
            print(f)
    return 0


def cmd_specgen(args) -> int:
    from .specgen.extract import ClaudeExtractor, ExtractionError, run

    if args.provider == "claude":
        extractor = ClaudeExtractor(model=args.model or "claude-opus-5-5", effort=args.effort)
        pages = args.pages_per_chunk or 25
    else:
        from .specgen.openai_compat import OpenAICompatibleExtractor

        if not args.model:
            print("--model is required with --provider openai (e.g. qwen2.5:14b for Ollama)", file=sys.stderr)
            return 2
        extractor = OpenAICompatibleExtractor(model=args.model, base_url=args.base_url, structured=args.structured)
        pages = args.pages_per_chunk or 8  # text models usually have smaller context windows
    try:
        spec, report, ok = run(args.pdf, args.venue_id, args.venue_name or args.venue_id, args.out,
                               extractor=extractor, pages_per_chunk=pages, workers=args.workers)
    except ExtractionError as e:
        print(f"extraction failed: {e}", file=sys.stderr)
        return 1
    print(f"spec:   {spec}\nreview: {report}")
    if not ok:
        print("spec has validation errors; see the review report", file=sys.stderr)
        return 2
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="fixgen")
    sub = p.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("validate", help="schema-check and lint venue specs")
    v.add_argument("specs", nargs="+")
    v.set_defaults(fn=cmd_validate)

    c = sub.add_parser("codegen", help="generate C++ venue adapters")
    c.add_argument("--out", required=True)
    c.add_argument("-v", "--verbose", action="store_true")
    c.add_argument("specs", nargs="+")
    c.set_defaults(fn=cmd_codegen)

    s = sub.add_parser("specgen", help="extract a venue spec from a PDF with an LLM")
    s.add_argument("pdf")
    s.add_argument("--venue-id", required=True, help="lower_snake_case id, becomes the file name")
    s.add_argument("--venue-name")
    s.add_argument("--out", default="specgen-out")
    s.add_argument("--provider", default="openai", choices=["openai", "claude"],
                   help="openai: any OpenAI-compatible server (Ollama, LM Studio, Gemini, Groq, ...); "
                        "claude: Anthropic API (needs ANTHROPIC_API_KEY)")
    s.add_argument("--base-url", default="ollama",
                   help="server URL or preset: ollama, lmstudio, gemini, groq, openrouter. "
                        "Hosted servers read the key from SPECGEN_API_KEY")
    s.add_argument("--model", help="model name on that server")
    s.add_argument("--structured", default="json_schema", choices=["json_schema", "json_object", "none"],
                   help="how strongly to constrain output; lower it if the server rejects the request")
    s.add_argument("--effort", default="high", choices=["low", "medium", "high", "xhigh", "max"],
                   help="claude only")
    s.add_argument("--pages-per-chunk", type=int, help="default 8 (openai) or 25 (claude)")
    s.add_argument("--workers", type=int, default=2, help="parallel chunks; use 1 for a local model")
    s.set_defaults(fn=cmd_specgen)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
