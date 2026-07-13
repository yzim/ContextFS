"""Benchmark records, aggregate totals, and stable on-disk result writers.

This module is shared by the sequential runner (:mod:`src.runner_regular`) and
the speculative runner (Task 6). It owns three things:

* The frozen dataclasses that describe one run: :class:`RunConfig` (inputs),
  :class:`StepRecord` (one row per ply), and :class:`RunOutcome` (the result of
  a full run, including the final :class:`~src.game.GameState`).
* :func:`compute_totals`, which reduces a run's step + verb records into an
  insertion-ordered dict (meta first, then aggregate counts and sums) that the
  CLI and the summary writer consume.
* :func:`write_results`, which lays down a stable directory of CSV / JSON /
  Markdown artifacts. Every file is always written -- including its header --
  so an empty-input run still produces a parseable directory with
  ``lossless.json["ok"] is True``.

The :class:`StepRecord` column set is fixed across modes: a regular-mode ply
sets every speculative column to its zero value (``spec_latency_s=0.0``,
``predicted_hit=False``, ``hit_idx=None``, ...) so the ``perstep.csv`` schema is
identical whether or not speculation was attempted.
"""

import csv
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from src.game import GameState, replay

# The per-step CSV column order. Frozen to the dataclass field declaration order
# so reordering StepRecord fields cannot silently reshuffle the CSV header: the
# header is derived from the dataclass itself, and this list documents the
# expected on-disk order at the top of the file for reviewers and downstream
# parsers.
PERSTEP_HEADER: tuple[str, ...] = (
    "ply", "source", "actor_latency_s", "spec_latency_s", "predictions",
    "predicted_hit", "prelaunched_hit", "hit_idx", "window_missed",
    "candidate_failures", "head_start_s", "latency_saved_s",
    "missed_window_wait_s", "actor_tokens_in", "actor_tokens_out",
    "spec_tokens_in", "spec_tokens_out", "candidate_tokens_in",
    "candidate_tokens_out", "candidate_token_usage_unknown", "committed_move",
)

# The verb CSV column order, matching src.agentvfs.VerbRecord.
VERB_HEADER: tuple[str, ...] = (
    "verb", "step", "latency_us", "status", "store_bytes_after",
)


@dataclass(frozen=True)
class RunConfig:
    """Inputs to a benchmark run.

    ``max_plies`` caps the total persisted trajectory; ``results_dir`` is where
    :func:`write_results` lays down artifacts; ``k`` / ``k_launch`` /
    ``candidate_timeout_s`` configure
    the speculative runner (Task 6) and are carried by the sequential runner for
    schema uniformity and so totals can record the configured window.
    """
    max_plies: int
    results_dir: Path
    k: int
    k_launch: int
    candidate_timeout_s: float


@dataclass(frozen=True)
class StepRecord:
    """One ply of a run, regular or speculative.

    Speculative columns are always present; regular-mode records set them to
    their zero values (see :func:`regular_step`). ``hit_idx`` is ``None`` when no
    candidate hit (regular mode, or a window miss). Candidate token fields count
    returned usage; ``candidate_token_usage_unknown`` counts launched workers
    canceled or failed before they could return usage.
    """
    ply: int
    source: str
    actor_latency_s: float
    spec_latency_s: float
    predictions: str
    predicted_hit: bool
    prelaunched_hit: bool
    hit_idx: int | None
    window_missed: bool
    candidate_failures: int
    head_start_s: float
    latency_saved_s: float
    missed_window_wait_s: float
    actor_tokens_in: int
    actor_tokens_out: int
    spec_tokens_in: int
    spec_tokens_out: int
    committed_move: str
    candidate_tokens_in: int = 0
    candidate_tokens_out: int = 0
    candidate_token_usage_unknown: int = 0


@dataclass(frozen=True)
class RunOutcome:
    """The result of a full run.

    ``state`` is the final :class:`GameState`; ``steps`` is one
    :class:`StepRecord` per ply actually played; ``wall_s`` is the measured wall
    time of the run; ``committed_moves`` is the full persisted trajectory,
    including moves loaded when resuming; ``totals`` is the aggregate dict from
    :func:`compute_totals`.
    """
    state: GameState
    steps: tuple[StepRecord, ...]
    wall_s: float
    committed_moves: tuple[str, ...]
    totals: dict


def regular_step(ply: int, move_uci: str, latency_s: float,
                 tokens_in: int = 0, tokens_out: int = 0) -> StepRecord:
    """Build a regular-mode :class:`StepRecord` with every speculative column
    zeroed. Centralizes the zero-value convention so the runner and the schema
    cannot drift."""
    return StepRecord(
        ply=ply,
        source="actor",
        actor_latency_s=latency_s,
        spec_latency_s=0.0,
        predictions="",
        predicted_hit=False,
        prelaunched_hit=False,
        hit_idx=None,
        window_missed=False,
        candidate_failures=0,
        head_start_s=0.0,
        latency_saved_s=0.0,
        missed_window_wait_s=0.0,
        actor_tokens_in=tokens_in,
        actor_tokens_out=tokens_out,
        spec_tokens_in=0,
        spec_tokens_out=0,
        committed_move=move_uci,
    )


def compute_totals(steps, verbs, wall_s: float,
                   meta: dict, verification_failures: int = 0) -> dict:
    """Reduce a run's step + verb records into an insertion-ordered totals dict.

    ``meta`` is copied in first (so ``mode`` and any other caller-supplied keys
    lead), followed by aggregate counts and the two summed speculative-save
    fields. Both summed fields are ``0.0`` for a regular run because every
    regular step zeros them.
    """
    step_list = list(steps)
    verb_list = list(verbs)
    totals: dict[str, Any] = dict(meta)
    totals["plies"] = len(step_list)
    totals["actor_steps"] = sum(1 for s in step_list if s.source == "actor")
    totals["spec_steps"] = sum(1 for s in step_list if s.source != "actor")
    totals["predicted_hits"] = sum(1 for s in step_list if s.predicted_hit)
    totals["prelaunched_hits"] = sum(1 for s in step_list if s.prelaunched_hit)
    is_spec = totals.get("mode") in {"spec", "speculative"}
    prediction_windows = (sum(1 for s in step_list if s.source != "prepared")
                          if is_spec else 0)
    totals["prediction_windows"] = prediction_windows
    totals["prediction_accuracy"] = (
        totals["predicted_hits"] / prediction_windows
        if prediction_windows else 0.0)
    totals["useful_hits"] = totals["prelaunched_hits"]
    totals["windows_missed"] = sum(1 for s in step_list if s.window_missed)
    totals["candidate_failures"] = sum(s.candidate_failures for s in step_list)
    totals["latency_saved_s"] = sum(s.latency_saved_s for s in step_list)
    totals["missed_window_wait_s"] = sum(s.missed_window_wait_s for s in step_list)
    totals["actor_tokens_in"] = sum(s.actor_tokens_in for s in step_list)
    totals["actor_tokens_out"] = sum(s.actor_tokens_out for s in step_list)
    totals["spec_tokens_in"] = sum(s.spec_tokens_in for s in step_list)
    totals["spec_tokens_out"] = sum(s.spec_tokens_out for s in step_list)
    totals["candidate_tokens_in"] = sum(
        s.candidate_tokens_in for s in step_list)
    totals["candidate_tokens_out"] = sum(
        s.candidate_tokens_out for s in step_list)
    totals["candidate_token_usage_unknown"] = sum(
        s.candidate_token_usage_unknown for s in step_list)
    totals["tokens_in_total"] = (totals["actor_tokens_in"]
                                 + totals["spec_tokens_in"]
                                 + totals["candidate_tokens_in"])
    totals["tokens_out_total"] = (totals["actor_tokens_out"]
                                  + totals["spec_tokens_out"]
                                  + totals["candidate_tokens_out"])
    totals["verbs"] = len(verb_list)
    totals["verb_failures"] = sum(
        1 for verb in verb_list if verb.status != "ok")
    totals["verification_failures"] = verification_failures
    totals["wall_s"] = wall_s
    return totals


def write_results(results_dir, steps, verbs, totals: dict,
                  state: GameState) -> None:
    """Write the stable results directory.

    Always writes every file -- including the header row -- so an empty-input
    run (no steps, no verbs, an initial state) still produces a parseable
    directory whose ``lossless.json`` reports ``ok: true`` (replaying zero moves
    from the initial fen reproduces the initial fen).

    Files:

    * ``perstep.csv``  -- one row per :class:`StepRecord`, header
      ``ply,source,...``.
    * ``verbs.csv``    -- one row per control verb, header
      ``verb,step,latency_us,status,store_bytes_after``.
    * ``totals.csv``   -- flat ``key,value`` rendering of ``totals`` in insertion
      order.
    * ``committed.json``  -- ``{"initial_fen": ..., "moves": [...]}``.
    * ``lossless.json``   -- ``{"ok", "final_fen", "replay_fen", "moves_match"}``;
      ``ok``/``moves_match`` are true iff replaying ``state.moves`` from
      ``state.initial_fen`` reproduces ``state.fen``.
    * ``summary.md``   -- ``totals`` rendered as a bulleted list in insertion
      order.
    """
    out = Path(results_dir)
    out.mkdir(parents=True, exist_ok=True)

    # perstep.csv -- header derived from PERSTEP_HEADER so the on-disk order is
    # fixed and self-documenting, independent of dataclass field order changes.
    with open(out / "perstep.csv", "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(PERSTEP_HEADER)
        for step in steps:
            writer.writerow([getattr(step, name) for name in PERSTEP_HEADER])

    # verbs.csv -- matches VerbRecord's fields. An empty verb list still yields
    # a header-only file so downstream parsers can always find the columns.
    with open(out / "verbs.csv", "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(VERB_HEADER)
        for verb in verbs:
            writer.writerow([getattr(verb, name) for name in VERB_HEADER])

    # totals.csv -- flat key/value in insertion order, so summary.md and
    # totals.csv agree on ordering.
    with open(out / "totals.csv", "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["key", "value"])
        for key, value in totals.items():
            writer.writerow([key, value])

    # committed.json -- the exact moves + starting fen, for replay anywhere.
    (out / "committed.json").write_text(json.dumps({
        "initial_fen": state.initial_fen,
        "moves": list(state.moves),
    }, sort_keys=True) + "\n")

    # lossless.json -- replay the committed moves and require fen equality.
    # moves_match and ok are the same predicate (fen equality after replay); we
    # surface both names so a consumer can read either without guessing.
    replayed = replay(state.initial_fen, state.moves)
    moves_match = replayed.fen == state.fen
    (out / "lossless.json").write_text(json.dumps({
        "ok": moves_match,
        "final_fen": state.fen,
        "replay_fen": replayed.fen,
        "moves_match": moves_match,
    }, sort_keys=True) + "\n")

    # summary.md -- render totals in insertion order as a bulleted list.
    lines = ["# Benchmark Summary", ""]
    for key, value in totals.items():
        lines.append(f"- {key}: {value}")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
