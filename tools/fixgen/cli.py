"""fixgen command line.

  fixgen validate spec/venues/*.yaml
  fixgen codegen --out build/generated spec/venues/*.yaml
  fixgen specgen --venue-id lse --venue-name "London Stock Exchange" spec.pdf
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

    try:
        spec, report, ok = run(args.pdf, args.venue_id, args.venue_name or args.venue_id, args.out,
                               extractor=ClaudeExtractor(model=args.model, effort=args.effort),
                               pages_per_chunk=args.pages_per_chunk, workers=args.workers)
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

    s = sub.add_parser("specgen", help="extract a venue spec from a PDF with Claude")
    s.add_argument("pdf")
    s.add_argument("--venue-id", required=True, help="lower_snake_case id, becomes the file name")
    s.add_argument("--venue-name")
    s.add_argument("--out", default="specgen-out")
    s.add_argument("--model", default="claude-opus-5-5")
    s.add_argument("--effort", default="high", choices=["low", "medium", "high", "xhigh", "max"])
    s.add_argument("--pages-per-chunk", type=int, default=25)
    s.add_argument("--workers", type=int, default=4)
    s.set_defaults(fn=cmd_specgen)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
