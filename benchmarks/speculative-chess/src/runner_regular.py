"""The sequential (non-speculative) benchmark runner.

:func:`run_regular` plays a game one ply at a time from a single actor -- no
speculation, no candidate branches -- and is the baseline against which the
speculative runner (Task 6) is measured. Each ply commits the actor's move to
``game.json`` via an atomic save and, when an :class:`~src.agentvfs.AgentVFS` is
provided, checkpoints the result so the run is recoverable. The final state is
replayed from the committed moves and required to be exactly equal to the
in-memory state before results are written.

All checkpoint calls go through the real ``AgentVFS.checkpoint(label, step=,
branch=)`` signature (see ``src/agentvfs.py``); the runner passes only ``label``
and ``step``, leaving ``branch`` at its default so checkpoints land on main.
"""

import time
from pathlib import Path

from src.game import GameState, replay
from src.llm import Actor
from src.metrics import (RunConfig, RunOutcome, StepRecord, compute_totals,
                         regular_step, write_results)


def run_regular(root, actor: Actor, cfg: RunConfig,
                avfs=None, verifier=None, mode: str = "regular") -> RunOutcome:
    """Play a sequential game and return the full :class:`RunOutcome`.

    ``root`` is the working directory holding ``game.json``; it is created if
    missing and seeded with :meth:`GameState.initial`. When ``avfs`` is provided
    the runner checkpoints ``chess-initial`` once before the loop and
    ``chess-ply-{N}`` after each ply N's atomic save, so each committed state is
    recoverable. When ``verifier`` is provided it snapshots main at the initial
    checkpoint and reads main back after each ply's commit, requiring the
    read-back state to equal the expected in-memory post-move state so a routing
    leak or save corruption is caught before it can corrupt the game.

    The loop resumes at the persisted ply and stops at the total
    ``cfg.max_plies`` limit or as soon as the chess result is terminal. At the
    end the full committed trajectory is replayed from the initial fen
    and required to reproduce the final :class:`GameState` exactly; the
    aggregate totals and the full results directory are written to
    ``cfg.results_dir``.
    """
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    game_path = root / "game.json"

    started = time.perf_counter()

    # Seed game.json if absent (resume semantics: an existing game.json is
    # honored). The state is then kept in memory and re-saved atomically per
    # ply, so the on-disk file and the in-memory state never diverge.
    if game_path.exists():
        state = GameState.load(game_path)
    else:
        state = GameState.initial()
        state.save(game_path)

    # Initial checkpoint + verifier baseline. The checkpoint lands on main
    # (branch left at its default); the snapshot establishes main's pre-loop
    # bytes so later per-ply checks have a reference.
    if avfs is not None:
        avfs.checkpoint("chess-initial", step=-1)
    if verifier is not None:
        verifier.snapshot_main()

    steps: list[StepRecord] = []
    committed: list[str] = list(state.moves)

    for ply in range(state.ply, cfg.max_plies):
        # Stop as soon as the game is decided -- a terminal position has a
        # non-"*" result (checkmate, stalemate, or a claimed draw).
        if state.result != "*":
            break

        position = state.position()
        move_result = actor.choose_move(position)
        state = state.apply(move_result.move)
        state.save(game_path)
        committed.append(move_result.move)

        if avfs is not None:
            avfs.checkpoint(f"chess-ply-{ply}", step=ply)

        # Per-ply read-back verify: read main game.json back through the
        # verifier and require it to equal the expected in-memory post-move
        # state. A divergence means FUSE/routing/save corruption mutated the
        # on-disk state between save and read, so we fail closed. This is NOT
        # check_main_unchanged: that "byte-identical since the window" semantics
        # is for speculative mode, where main must never change during a window.
        # Regular mode mutates main every ply (the commit IS the move), so the
        # meaningful check is read-back GameState vs the expected post-move
        # state -- a real, non-tautological comparison against an independent
        # reference, not the file compared to itself.
        if verifier is not None:
            read_back = verifier.snapshot_main()
            if read_back != state:
                raise RuntimeError(
                    f"main read-back diverged from expected post-move state "
                    f"at ply {ply}: read fen {getattr(read_back, 'fen', read_back)!r} "
                    f"!= expected fen {state.fen!r}")

        steps.append(regular_step(
            ply=ply,
            move_uci=move_result.move,
            latency_s=move_result.latency_s,
            tokens_in=move_result.tokens_in,
            tokens_out=move_result.tokens_out,
        ))

    wall_s = time.perf_counter() - started

    # Lossless postcondition: replaying the committed moves from the initial fen
    # must reproduce the final state exactly (fen, ply, result, moves). This
    # guards against any drift between the in-memory state and the persisted
    # game.json across the ply loop.
    replayed = replay(state.initial_fen, committed)
    if replayed != state:
        raise RuntimeError(
            "regular runner lossless check failed: "
            f"replayed fen {replayed.fen!r} != state fen {state.fen!r}")

    verbs = avfs.verbs if avfs is not None else []
    verify_failures = verifier.failures if verifier is not None else 0
    totals = compute_totals(
        steps, verbs, wall_s, {"mode": mode},
        verification_failures=verify_failures)
    write_results(cfg.results_dir, steps, verbs, totals, state)

    return RunOutcome(
        state=state,
        steps=tuple(steps),
        wall_s=wall_s,
        committed_moves=tuple(committed),
        totals=totals,
    )
