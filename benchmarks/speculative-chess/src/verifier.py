"""Routed-cgroup semantic postcondition checks for the speculative runner.

The :class:`Verifier` observes main and per-candidate state THROUGH the
registered candidate cgroup (before and after launch), so a routing leak or a
stray main mutation is caught before it can corrupt the game. Every failure
writes a bounded diagnostic (``results/verify-fail-<label>.txt``) with the
expected and actual state, appends ``status()`` / ``branch_list()`` output
when the daemon is reachable, and raises :class:`VerificationError` so the
runner fails closed.

All file observation goes through ``cgroup.run_text(["cat", ...], cwd=mount)``
to exercise the same routing path the worker writes through -- reading main
directly from the mount would miss a routing bug.
"""

import json
from pathlib import Path
from typing import Iterable, Optional

from src.candidate_worker import CandidateResult, PreparedResponse
from src.game import GameState, StateError


class VerificationError(RuntimeError):
    """A speculative postcondition failed; see ``verify-fail-<label>.txt``."""


class Verifier:
    """Records main's pre-window state and re-checks it through routed cgroups.

    ``failures`` counts how many postconditions have failed so far (the runner
    surfaces it as ``verify_failures``); the byte snapshot of main is captured
    once by :meth:`snapshot_main` and compared by the later checks.
    """

    def __init__(self, avfs, mount: Path, results_dir: Path):
        self.avfs = avfs
        self.mount = Path(mount)
        self.results_dir = Path(results_dir)
        self.failures = 0
        self._main_bytes: Optional[bytes] = None
        self._main_state: Optional[GameState] = None

    # --- snapshot ------------------------------------------------------------

    def snapshot_main(self) -> GameState:
        """Record main's game.json bytes and parsed state before a window."""
        path = self.mount / "game.json"
        self._main_bytes = path.read_bytes()
        self._main_state = GameState.load(path)
        return self._main_state

    # --- checks --------------------------------------------------------------

    def check_main_unchanged(self) -> None:
        """Main game.json must be byte-identical to the snapshot: speculation
        must never mutate main except through an explicit merge/checkpoint."""
        self._require_snapshot("check_main_unchanged")
        assert self._main_bytes is not None
        current = (self.mount / "game.json").read_bytes()
        if current != self._main_bytes:
            self._fail("main-mutated", [
                "main game.json changed during the speculation window",
                f"snapshot bytes: {self._main_bytes!r}",
                f"current bytes:  {current!r}",
            ])

    def check_candidate_base(self, attempt) -> None:
        """Before launch, the candidate cgroup's game.json must byte-match the
        main snapshot -- proving the new branch started from main's exact
        state and routing shows main's view to the unlaunched candidate."""
        self._require_snapshot("check_candidate_base")
        assert self._main_bytes is not None
        observed = attempt.cgroup.run_text(
            ["cat", "game.json"], cwd=str(self.mount))
        if observed.encode() != self._main_bytes:
            self._fail("candidate-base", [
                "candidate branch game.json does not byte-match main",
                f"main bytes:     {self._main_bytes!r}",
                f"candidate text: {observed!r}",
            ])

    def check_candidate(self, attempt, result: CandidateResult) -> None:
        """After a candidate completes, read game.json + prepared_response.json
        THROUGH its cgroup so the routed branch is what we verify. The
        candidate's game.json must equal main + result.predicted_move, and the
        prepared response must validate against that candidate state."""
        self._require_snapshot("check_candidate")
        assert self._main_state is not None
        text = attempt.cgroup.run_text(
            ["cat", "game.json", "prepared_response.json"],
            cwd=str(self.mount))
        docs = [ln for ln in text.splitlines() if ln.strip()]
        if len(docs) != 2:
            self._fail("candidate-read", [
                "expected 2 documents (game.json, prepared_response.json) "
                f"through the candidate cgroup, got {len(docs)}",
                f"raw: {text!r}",
            ])
        try:
            cand_state = GameState.from_dict(json.loads(docs[0]))
            prepared = PreparedResponse.from_dict(json.loads(docs[1]))
        # Broaden to Exception: GameState.from_dict is called directly (not via
        # the safe GameState.load wrapper), so a corrupt candidate game.json
        # can raise KeyError (missing key) / TypeError (wrong type) /
        # AttributeError -- none in the old tuple. Fail closed (via _fail) but
        # keep the diagnostic dump instead of escaping as a raw exception.
        except Exception as exc:
            self._fail("candidate-read", [
                f"could not parse routed candidate files: {exc}",
                f"raw: {text!r}",
            ])
        expected = self._main_state.apply(result.predicted_move)
        if cand_state.fen != expected.fen:
            self._fail("candidate-state", [
                "candidate game.json is not main + predicted_move",
                f"predicted_move:     {result.predicted_move}",
                f"expected fen (main+): {expected.fen}",
                f"candidate fen:        {cand_state.fen}",
            ])
        try:
            prepared.validate_against(cand_state)
        except StateError as exc:
            self._fail("candidate-prepared", [
                "prepared response failed validation against the candidate state",
                f"prepared: {prepared.to_dict()}",
                f"reason:   {exc}",
            ])

    def check_hit_merge(self, attempt) -> None:
        """After the winner is merged into main, main's game.json must equal
        what the candidate cgroup still observes -- the merge committed the
        candidate's predicted state to main."""
        main_now = (self.mount / "game.json").read_bytes()
        branch_text = attempt.cgroup.run_text(
            ["cat", "game.json"], cwd=str(self.mount))
        if main_now != branch_text.encode():
            self._fail("hit-merge", [
                "main does not reflect the merged candidate branch",
                f"main bytes:   {main_now!r}",
                f"branch bytes: {branch_text.encode()!r}",
            ])

    def check_cleanup(self, attempts: Iterable, branch_names: Iterable[str]) -> None:
        """Every candidate branch is gone from branch_list(), every attempt
        reports no live process, and every candidate cgroup path is absent."""
        live = {b["name"] for b in self.avfs.branch_list()}
        leftover = sorted(n for n in branch_names if n in live)
        if leftover:
            self._fail("cleanup-branches", [
                "candidate branches still present after cleanup",
                f"leftover: {leftover}",
            ])
        for attempt in attempts:
            if attempt.process_live:
                self._fail("cleanup-process", [
                    f"candidate {attempt.branch} process still live",
                ])
            if attempt.cgroup is not None and Path(attempt.cgroup.path).exists():
                self._fail("cleanup-cgroup", [
                    f"candidate cgroup remains: {attempt.cgroup.path}",
                ])

    # --- internals -----------------------------------------------------------

    def _require_snapshot(self, caller: str) -> None:
        if self._main_bytes is None or self._main_state is None:
            self._fail("main-snapshot-missing", [
                f"{caller} called before snapshot_main()",
            ])

    def _daemon_diagnostics(self) -> list[str]:
        """Status + branch_list output, when the daemon is reachable. Returns
        [] on any error so an unreachable daemon never masks the original
        failure or leaks transport detail into the report."""
        try:
            return [
                f"status: {json.dumps(self.avfs.status())}",
                f"branches: {json.dumps(self.avfs.branch_list())}",
            ]
        except Exception:
            return []

    def _fail(self, label: str, lines: list[str]) -> None:
        self.failures += 1
        self.results_dir.mkdir(parents=True, exist_ok=True)
        path = self.results_dir / f"verify-fail-{label}.txt"
        body = [f"POSTCONDITION FAILED: {label}", *lines]
        diag = self._daemon_diagnostics()
        if diag:
            body.append("")
            body.extend(diag)
        body.append("")
        path.write_text("\n".join(body))
        raise VerificationError(f"{label}: see {path}")
