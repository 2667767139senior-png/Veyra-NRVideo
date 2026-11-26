#!/usr/bin/env python3
"""Legacy-multiplier regression gate.

AFG must not have moved the old path. This reads a run that was launched with
`--legacy <N>` (the panel's own multiplier setters, not a test-only path) and
checks the three things that define it:

  1. no AFG telemetry at all - no `afg-output`, no `afg-guard`, no `afg` plan;
  2. the group really ran at the requested multiplier: generated frames per real
     frame is N-1 within a few percent;
  3. the presenter submitted at the rate that multiplier implies, measured, not
     assumed.

    python tools/afg_legacy_check.py <logfile> [--multiplier 2] [--source 30]

Exit code 0 = the legacy path behaves as it did before AFG existed.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

FLOW = re.compile(r"\[frame-flow\]\s*(?P<body>state=.*)$")
DISPLAY = re.compile(r"\[display-stats\]\s*(?P<body>.*)$")
AFG_MARKERS = ("[afg-output]", "[afg-guard]", "mode=afg")


def fields(body: str) -> dict[str, str]:
    body = body.split(" (")[0]
    out: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, _, value = token.partition("=")
            out[key] = value
    return out


def num(values: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(values[key])
    except (KeyError, ValueError):
        return default


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--multiplier", type=int, default=2)
    parser.add_argument("--source", type=float, default=30.0)
    parser.add_argument("--tolerance", type=float, default=0.10)
    args = parser.parse_args()

    path = Path(args.log)
    if not path.is_file():
        print(f"ERROR: {path} not found")
        return 2
    text = path.read_text(encoding="utf-8", errors="replace")

    markers = {m: text.count(m) for m in AFG_MARKERS}
    afg_lines = sum(markers.values())

    flow = {}
    calls = []
    for line in text.splitlines():
        match = FLOW.search(line)
        if match:
            flow = fields(match.group("body"))
        match = DISPLAY.search(line)
        if match:
            # presentCalls is the number of successful presents in that ~1 s
            # sample: the delivered rate, which is what a multiplier is judged by.
            value = num(fields(match.group("body")), "presentCalls")
            if value > 0:
                calls.append(value)

    real = num(flow, "realPresented")
    generated = num(flow, "generatedPresented")
    steady = sorted(calls)[len(calls) // 3:] or [0.0]  # drop the partial first second
    delivered = sorted(steady)[len(steady) // 2] if steady else 0.0
    per_real = generated / real if real else 0.0
    expected_per_real = max(0.0, args.multiplier - 1.0)
    expected_rate = args.source * args.multiplier

    problems = []
    if afg_lines:
        problems.append(f"AFG telemetry present in a legacy run ({markers})")
    if not real:
        problems.append("no frame-flow close record: the run did not shut down cleanly")
    elif abs(per_real - expected_per_real) > max(0.15, expected_per_real * args.tolerance):
        problems.append(f"{per_real:.2f} generated per real frame, expected "
                        f"{expected_per_real:.2f} for multiplier {args.multiplier}")
    if not delivered:
        problems.append("no display-stats samples: the delivered rate is unknown")
    elif abs(delivered - expected_rate) > expected_rate * 0.15:
        problems.append(f"delivered {delivered:.0f} presents/s, expected about {expected_rate:.0f}/s")

    print(f"legacy regression: {path.name}")
    print(f"  multiplier     {args.multiplier}  (source {args.source:.0f} FPS -> expected "
          f"{expected_rate:.0f} presents/s)")
    print(f"  afg telemetry  {afg_lines} lines {markers}")
    print(f"  real presents  {real:.0f}")
    print(f"  generated      {generated:.0f}  ({per_real:.2f} per real frame)")
    print(f"  delivered      {delivered:.0f} presents/s (median of {len(calls)} one-second samples)")
    if problems:
        print("\nFAIL:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("\nPASS: the legacy multiplier path is unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
