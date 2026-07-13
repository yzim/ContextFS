"""Deterministic scripted candidate worker for the AgentVFS integration test.

Reads a JSON map ``{predicted_move: prepared_move | "FAIL"}`` from the
``CHESS_SCRIPTED_RESPONSES`` environment variable. For the candidate's
``--predicted-move``:

* if the mapped value is the string ``"FAIL"``, the worker exits nonzero
  (the runner treats nonzero exit as a :class:`CandidateWorkError` and keeps
  launching healthy siblings);
* otherwise it builds a single-reply :class:`ScriptedActor` for the mapped
  prepared move and calls :func:`run_candidate`, emitting exactly one
  serialized :class:`CandidateResult` JSON line on stdout.

This keeps every integration worker I/O on the routed mount without any network
access: no OpenAI client, no model key, no transport. Resolved secrets never
appear on argv or stdout.
"""

import argparse
import json
import os
import sys
from dataclasses import asdict
from pathlib import Path

from src.candidate_worker import run_candidate
from tests.fakes import ScriptedActor


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="scripted_worker")
    parser.add_argument("--root", required=True)
    parser.add_argument("--predicted-move", required=True)
    # parse_known_args tolerates any extra flags a shared worker prefix might
    # carry, so the same CandidateFactory prefix shape works for the real and
    # the scripted worker.
    args, _unknown = parser.parse_known_args(argv)

    try:
        responses = json.loads(os.environ["CHESS_SCRIPTED_RESPONSES"])
    except (KeyError, json.JSONDecodeError) as exc:
        sys.stderr.write(
            f"scripted_worker: bad CHESS_SCRIPTED_RESPONSES: {exc}\n")
        return 2

    predicted = args.predicted_move
    if predicted not in responses:
        sys.stderr.write(
            f"scripted_worker: no scripted response for {predicted}\n")
        return 2
    prepared = responses[predicted]
    if prepared == "FAIL":
        # Candidate-local failure: the runner drops this candidate and keeps
        # launching healthy siblings.
        sys.stderr.write(f"scripted_worker: scripted FAIL for {predicted}\n")
        return 1

    actor = ScriptedActor.moves([prepared])
    try:
        result = run_candidate(Path(args.root), predicted, actor)
    except Exception as exc:
        sys.stderr.write(f"scripted_worker: run_candidate failed: {exc}\n")
        return 3
    sys.stdout.write(json.dumps(asdict(result)) + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised via subprocess
    sys.exit(main())
