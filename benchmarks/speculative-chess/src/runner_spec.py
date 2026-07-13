"""The speculative benchmark runner: one bounded per-ply race, lossless hits.

:func:`run_spec` plays a game ply-by-ply. Each ply submits the authoritative
:class:`Actor` move and the :class:`Speculator`'s top-``k`` predictions to a
``ThreadPoolExecutor(max_workers=2)`` for exactly one window. If the Speculator
finishes first it creates, base-verifies, and launches up to ``cfg.k_launch``
candidate workers (each on its own branch); after the authoritative Actor move
arrives, a matching LAUNCHED candidate's prepared reply is merged into main and
consumed by the next ply, saving the Actor's thinking time on that ply. If the
Actor finishes first the window is missed: no candidates are launched, the
bounded Speculator future is joined at block exit, and the Actor move is
applied sequentially like the regular runner.

The runner is *lossless*: every committed move reaches ``game.json`` exactly
once, and the final state is replayed from the committed moves and required to
be byte-equal to the in-memory state before results are written. It is also
*fail-closed*: :class:`CtlError`, :class:`VerificationError`, :class:`CleanupError`,
and :class:`StateError` are never caught as workload degradation -- they
propagate and abort. The single outer exception handler retries cleanup for
every live candidate attempt and then re-raises the original correctness error
(chaining a :class:`CleanupError` only if resources survived the retry).

Malformed model output is diagnosed but never leaks request material:
:class:`LLMOutputError.raw_excerpt` (already capped and sanitized by the LLM
layer) is written to ``results/invalid-<role>-ply<N>.txt``. A malformed
Speculator degrades to the Actor move (no abort); a malformed Actor writes its
excerpt and aborts.
"""

import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from dataclasses import dataclass
from pathlib import Path

from src.candidate_worker import (CandidateAttempt, CandidateWorkError,
                                  CleanupError, PreparedResponse)
from src.game import GameState, StateError, replay
from src.llm import Actor, LLMOutputError, Speculator
from src.metrics import (RunConfig, RunOutcome, StepRecord, compute_totals,
                         write_results)

# Mirrors src.llm._RAW_EXCERPT_CAP so a hand-built error's excerpt is capped the
# same way a real LLMOutputError's is. The LLM layer already caps raw_excerpt on
# construction; the slice here is defense-in-depth for test-built errors.
_EXCERPT_CAP = 4096


@dataclass(frozen=True)
class _CompletedCall:
    value: object | None
    error: BaseException | None
    completed_at: float


def _capture_call(call, *args) -> _CompletedCall:
    """Run one model call and stamp completion in the worker thread.

    The timestamp is captured where the call actually finishes, not later when
    the coordinator observes the Future. Errors are carried back as values so
    simultaneous completions can still be ordered before role-specific error
    handling runs.
    """
    try:
        return _CompletedCall(call(*args), None, time.monotonic())
    except BaseException as exc:
        return _CompletedCall(None, exc, time.monotonic())


def consume_prepared(root: Path) -> str:
    """Apply a hit's staged prepared reply and return the applied move.

    Loads ``game.json`` (the predicted move the merge committed) and the staged
    ``prepared_response.json``, validates the reply is still legal in that
    state, applies it atomically, unlinks the staged file, and returns the
    reply. Raises :class:`StateError` on any mismatch (fail closed).
    """
    state = GameState.load(root / "game.json")
    prepared = PreparedResponse.load(root / "prepared_response.json")
    prepared.validate_against(state)
    state.apply(prepared.prepared_move).save(root / "game.json")
    (root / "prepared_response.json").unlink()
    return prepared.prepared_move


def require_cleanup(attempts: list[CandidateAttempt]) -> None:
    """Tear down every attempt in ``attempts`` with a bounded two-pass retry,
    then clear the list. Raises :class:`CleanupError` naming any branches whose
    resources survived both passes (so an outer handler can finish the job)."""
    unfinished = list(attempts)
    for _ in range(2):
        unfinished = [attempt for attempt in unfinished
                      if not attempt.cleanup()]
        if not unfinished:
            break
    if unfinished:
        names = ", ".join(attempt.branch for attempt in unfinished)
        raise CleanupError("candidate cleanup failed: " + names)
    attempts.clear()


def _write_excerpt(role: str, exc: LLMOutputError, ply: int,
                   results_dir: Path) -> None:
    """Write the bounded, sanitized model-text excerpt to disk. The excerpt is
    already capped/sanitized by the LLM layer; the slice is defense-in-depth."""
    results_dir.mkdir(parents=True, exist_ok=True)
    (results_dir / f"invalid-{role}-ply{ply}.txt").write_text(
        exc.raw_excerpt[:_EXCERPT_CAP])


def _resolve_speculator(outcome: _CompletedCall, ply: int, results_dir: Path):
    """Resolve a completed speculator call, degrading ordinary failures.

    On :class:`LLMOutputError` the bounded excerpt is written to
    ``invalid-speculator-ply<N>.txt`` and empty predictions are returned (the
    Speculator is treated as degraded). On any ordinary exception the Speculator
    is likewise degraded. Process-control exceptions such as KeyboardInterrupt
    propagate so outer cleanup still runs. Returns
    ``(moves, latency_s, tokens_in, tokens_out)``.
    """
    if isinstance(outcome.error, LLMOutputError):
        exc = outcome.error
        _write_excerpt("speculator", exc, ply, results_dir)
        return (), exc.latency_s, exc.tokens_in, exc.tokens_out
    if isinstance(outcome.error, Exception):
        # Transport / unexpected failure: degraded Speculator, no predictions.
        return (), 0.0, 0, 0
    if outcome.error is not None:
        raise outcome.error
    pred = outcome.value
    return tuple(pred.moves), pred.latency_s, pred.tokens_in, pred.tokens_out


def _resolve_actor(outcome: _CompletedCall, ply: int, results_dir: Path):
    if isinstance(outcome.error, LLMOutputError):
        _write_excerpt("actor", outcome.error, ply, results_dir)
    if outcome.error is not None:
        raise outcome.error
    return outcome.value


def run_spec(root, actor: Actor, speculator: Speculator, cfg: RunConfig,
             avfs, verifier, candidate_factory) -> RunOutcome:
    """Play a speculative game and return the full :class:`RunOutcome`.

    Per ply: consume any prepared response staged by a prior hit (source
    ``"prepared"``); otherwise open one bounded two-future race. On a
    Speculator win, create + base-verify + launch up to ``cfg.k_launch``
    candidates, then await the Actor. A healthy matching candidate is merged
    into main, its prepared reply consumed next ply, and ``latency_saved_s``
    recorded. Every other outcome cleans all live candidates, verifies main is
    untouched by speculation, and applies + checkpoints the Actor move
    sequentially. See the module docstring for the fail-closed taxonomy.
    """
    root = Path(root)
    game_path = root / "game.json"
    results_dir = Path(cfg.results_dir)
    root.mkdir(parents=True, exist_ok=True)

    started = time.perf_counter()

    if game_path.exists():
        state = GameState.load(game_path)
    else:
        state = GameState.initial()
        state.save(game_path)

    if avfs is not None:
        avfs.checkpoint("chess-initial", step=-1)
    verifier.snapshot_main()

    steps: list[StepRecord] = []
    committed: list[str] = list(state.moves)
    live: list[CandidateAttempt] = []

    try:
        for ply in range(state.ply, cfg.max_plies):
            if state.result != "*":
                break

            # Consume a prepared reply staged by a prior ply's hit. This ply's
            # "move" is the prepared reply, already validated+applied by
            # consume_prepared; no speculation window opens.
            if (root / "prepared_response.json").exists():
                move = consume_prepared(root)
                state = GameState.load(game_path)
                committed.append(move)
                if avfs is not None:
                    avfs.checkpoint(f"chess-ply-{ply}", step=ply)
                steps.append(StepRecord(
                    ply=ply, source="prepared", actor_latency_s=0.0,
                    spec_latency_s=0.0, predictions="", predicted_hit=False,
                    prelaunched_hit=False, hit_idx=None, window_missed=False,
                    candidate_failures=0, head_start_s=0.0,
                    latency_saved_s=0.0, missed_window_wait_s=0.0,
                    actor_tokens_in=0, actor_tokens_out=0,
                    spec_tokens_in=0, spec_tokens_out=0,
                    committed_move=move))
                verifier.snapshot_main()  # re-baseline after main mutated
                continue

            position = state.position()
            window_missed = False
            head_start_s = 0.0
            missed_window_wait_s = 0.0
            predictions: tuple[str, ...] = ()
            spec_latency = 0.0
            spec_tokens_in = 0
            spec_tokens_out = 0
            candidate_failures = 0
            candidate_tokens_in = 0
            candidate_tokens_out = 0
            candidate_token_usage_unknown = 0
            launched: list[CandidateAttempt] = []

            # --- one bounded per-ply race ---
            with ThreadPoolExecutor(max_workers=2) as pool:
                actor_future = pool.submit(
                    _capture_call, actor.choose_move, position)
                spec_future = pool.submit(
                    _capture_call, speculator.predict_moves, position, cfg.k)
                done, _not_done = wait(
                    {actor_future, spec_future},
                    return_when=FIRST_COMPLETED)

                actor_outcome = (actor_future.result()
                                 if actor_future in done else None)
                spec_outcome = (spec_future.result()
                                if spec_future in done else None)
                if actor_outcome is not None and spec_outcome is not None:
                    spec_won = (spec_outcome.completed_at
                                < actor_outcome.completed_at)
                else:
                    spec_won = spec_outcome is not None

                if spec_won:
                    assert spec_outcome is not None
                    spec_done_at = spec_outcome.completed_at
                    (predictions, spec_latency, spec_tokens_in,
                     spec_tokens_out) = _resolve_speculator(
                        spec_outcome, ply, results_dir)
                    predictions = predictions[:cfg.k]
                    for idx, pmv in enumerate(predictions[:cfg.k_launch]):
                        attempt = None
                        try:
                            attempt = candidate_factory.create(
                                step=ply, index=idx, predicted_move=pmv)
                            # Append BEFORE base-verify/launch so a verifier or
                            # work exception cannot leak the attempt from
                            # cleanup.
                            live.append(attempt)
                            verifier.check_candidate_base(attempt)
                            attempt.launch()
                            launched.append(attempt)
                        except CandidateWorkError:
                            # Candidate-local failure. When create() itself
                            # raised, the factory's _abort already cleaned its
                            # partial state, so only a RETURNED attempt
                            # (base-verify or launch failure) needs cleanup
                            # here. Drop it and keep launching healthy
                            # siblings. A CleanupError from create's own
                            # failed cleanup is NOT caught here -- it
                            # propagates fail-closed.
                            if attempt is not None:
                                require_cleanup([attempt])
                                if attempt in live:
                                    live.remove(attempt)
                            candidate_failures += 1
                    verifier.check_main_unchanged()
                    if actor_outcome is None:
                        actor_outcome = actor_future.result()
                    actor_move_result = _resolve_actor(
                        actor_outcome, ply, results_dir)
                    actor_done_at = actor_outcome.completed_at
                    head_start_s = max(0.0, actor_done_at - spec_done_at)
                else:
                    # Actor finished first: the window was missed. Create no
                    # branches; the bounded Speculator future is joined at the
                    # `with` block exit, then resolved below for diagnostics.
                    if actor_outcome is None:
                        actor_outcome = actor_future.result()
                    actor_done_at = actor_outcome.completed_at
                    window_missed = True
                    actor_move_result = _resolve_actor(
                        actor_outcome, ply, results_dir)

            actor_move = actor_move_result.move

            # Actor-first: the speculator was joined at block exit. Measure the
            # extra wait and resolve it (writes the excerpt on LLMOutputError;
            # valid predictions are scored but NOT launched -- window missed).
            if window_missed:
                if spec_outcome is None:
                    spec_outcome = spec_future.result()
                spec_done_at = spec_outcome.completed_at
                missed_window_wait_s = max(0.0,
                                           spec_done_at - actor_done_at)
                (predictions, spec_latency, spec_tokens_in,
                 spec_tokens_out) = _resolve_speculator(
                    spec_outcome, ply, results_dir)
                predictions = predictions[:cfg.k]

            predicted_hit = actor_move in predictions
            prelaunched_hit = False
            hit_idx = None
            latency_saved_s = 0.0

            # Scan only LAUNCHED candidates for a match against the Actor move.
            match = None
            for attempt in launched:
                if attempt.predicted_move == actor_move:
                    match = attempt
                    hit_idx = predictions.index(actor_move)
                    break

            if match is not None and predicted_hit:
                # Collect the prepared reply. A CandidateWorkError here means
                # the match is unhealthy -> fall through to the miss path. A
                # VerificationError from check_candidate propagates (fail
                # closed): a routing/correctness bug must abort, not degrade.
                cand_result = None
                try:
                    cand_result = match.collect(cfg.candidate_timeout_s)
                except CandidateWorkError:
                    cand_result = None
                    candidate_failures += 1
                else:
                    verifier.check_candidate(match, cand_result)

                if cand_result is not None:
                    cleaned_attempts = list(launched)
                    siblings = [a for a in launched if a is not match]
                    if siblings:
                        require_cleanup(siblings)
                        for sib in siblings:
                            if sib in live:
                                live.remove(sib)
                    verifier.check_main_unchanged()
                    avfs.branch_merge(match.branch, into="main", step=ply)
                    verifier.check_hit_merge(match)
                    # latency_saved is bounded by both the candidate's own work
                    # time (cand_result.latency_s) and the head start the
                    # speculator bought (head_start_s): the winner's prepared
                    # reply is only "free" up to whichever is smaller.
                    latency_saved_s = min(cand_result.latency_s, head_start_s)
                    candidate_tokens_in = cand_result.tokens_in
                    candidate_tokens_out = cand_result.tokens_out
                    candidate_token_usage_unknown = max(0, len(launched) - 1)
                    require_cleanup([match])
                    if match in live:
                        live.remove(match)
                    verifier.check_cleanup(
                        cleaned_attempts, [a.branch for a in cleaned_attempts])
                    prelaunched_hit = True
                    # The merge committed the candidate's prepared state to
                    # main (main = main + actor_move); do NOT re-apply the
                    # Actor move. Reload the merged state.
                    state = GameState.load(game_path)
                    committed.append(actor_move)
                    # Spec: the hit's merge commit is its durable commit and
                    # is NOT followed by a redundant checkpoint -- emitting
                    # chess-ply-{ply} here would inflate spec-mode verb/store
                    # metrics in the regular-vs-spec comparison.
                    steps.append(StepRecord(
                        ply=ply, source="spec",
                        actor_latency_s=actor_move_result.latency_s,
                        spec_latency_s=spec_latency,
                        predictions=",".join(predictions),
                        predicted_hit=True, prelaunched_hit=True,
                        hit_idx=hit_idx, window_missed=False,
                        candidate_failures=candidate_failures,
                        head_start_s=head_start_s,
                        latency_saved_s=latency_saved_s,
                        missed_window_wait_s=0.0,
                        actor_tokens_in=actor_move_result.tokens_in,
                        actor_tokens_out=actor_move_result.tokens_out,
                        spec_tokens_in=spec_tokens_in,
                        spec_tokens_out=spec_tokens_out,
                        committed_move=actor_move,
                        candidate_tokens_in=candidate_tokens_in,
                        candidate_tokens_out=candidate_tokens_out,
                        candidate_token_usage_unknown=(
                            candidate_token_usage_unknown)))
                    verifier.snapshot_main()  # re-baseline after the merge
                    continue

            # --- miss path (no match, unhealthy match, or window missed) ---
            cleaned = list(live)
            require_cleanup(live)  # clears `live`
            verifier.check_cleanup(cleaned, [a.branch for a in cleaned])
            verifier.check_main_unchanged()
            candidate_token_usage_unknown = len(launched)
            state = state.apply(actor_move)
            state.save(game_path)
            committed.append(actor_move)
            if avfs is not None:
                avfs.checkpoint(f"chess-ply-{ply}", step=ply)
            steps.append(StepRecord(
                ply=ply, source="actor",
                actor_latency_s=actor_move_result.latency_s,
                spec_latency_s=spec_latency,
                predictions=",".join(predictions),
                predicted_hit=predicted_hit, prelaunched_hit=False,
                hit_idx=None, window_missed=window_missed,
                candidate_failures=candidate_failures,
                head_start_s=head_start_s, latency_saved_s=0.0,
                missed_window_wait_s=missed_window_wait_s,
                actor_tokens_in=actor_move_result.tokens_in,
                actor_tokens_out=actor_move_result.tokens_out,
                spec_tokens_in=spec_tokens_in,
                spec_tokens_out=spec_tokens_out,
                committed_move=actor_move,
                candidate_tokens_in=candidate_tokens_in,
                candidate_tokens_out=candidate_tokens_out,
                candidate_token_usage_unknown=candidate_token_usage_unknown))
            verifier.snapshot_main()  # re-baseline after the sequential commit
    except BaseException as original:
        # Outer correctness-error handler: retry cleanup for EVERY live
        # attempt, then re-raise the original error. Chain a CleanupError only
        # if resources survived the retry. CtlError / VerificationError /
        # CleanupError / StateError / LLMOutputError all reach here and are
        # re-raised (never swallowed as workload degradation).
        leftover = [attempt for attempt in live if not attempt.cleanup()]
        if leftover:
            names = ", ".join(attempt.branch for attempt in leftover)
            raise original from CleanupError(
                "candidate cleanup failed: " + names)
        raise

    wall_s = time.perf_counter() - started

    # Lossless postcondition: replay the committed moves from the initial fen
    # and require exact equality with the final in-memory state.
    replayed = replay(state.initial_fen, committed)
    if replayed != state:
        raise RuntimeError(
            "speculative runner lossless check failed: "
            f"replayed fen {replayed.fen!r} != state fen {state.fen!r}")

    verbs = avfs.verbs if avfs is not None else []
    totals = compute_totals(
        steps, verbs, wall_s, {"mode": "spec"},
        verification_failures=verifier.failures)
    write_results(results_dir, steps, verbs, totals, state)

    return RunOutcome(
        state=state,
        steps=tuple(steps),
        wall_s=wall_s,
        committed_moves=tuple(committed),
        totals=totals,
    )
