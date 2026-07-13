"""Candidate worker, attempt lifecycle, and routed cleanup.

A *candidate worker* is spawned inside a per-candidate cgroup whose session is
registered with the AgentVFS daemon, so every file write the worker makes
lands on a private branch instead of main. :func:`run_candidate` is the
in-process entry the worker CLI calls: it loads the authoritative game state,
applies the speculator's *predicted* move, asks the opposing :class:`Actor`
for a reply on the resulting position, validates that reply, and atomically
writes the predicted state plus a :class:`PreparedResponse` so the runner can
consume it on a hit without re-asking the model. Only the predicted move is
written to ``game.json`` -- the prepared reply is staged in
``prepared_response.json`` and applied later by the runner once the prediction
is confirmed correct.

The attempt lifecycle (:class:`CandidateAttempt` / :class:`CandidateFactory`)
owns four resources in a strict order -- process, session, cgroup, branch --
and :meth:`CandidateAttempt.cleanup` tears them down in the reverse
``kill -> unregister -> destroy -> delete`` order, retrying only the steps
that did not yet succeed so an outer exception handler can finish the job.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Optional

from src.agentvfs import AgentVFS, CtlError
from src.cgroup import CgroupSession
from src.game import GameState, StateError
from src.llm import Actor, LLMOutputError, OpenAIActor


class CandidateWorkError(RuntimeError):
    """Worker spawn, exit, timeout, or result-protocol failure."""


class CleanupError(RuntimeError):
    """A candidate resource survived bounded cleanup."""


# --- prepared response + candidate result -----------------------------------
# Both are single-line compact JSON on disk so a routed `cat game.json
# prepared_response.json` yields exactly two parseable records.


@dataclass(frozen=True)
class PreparedResponse:
    """The reply staged for a predicted move: validate-then-apply on a hit.

    ``position_fen`` is the FEN of the state the candidate wrote (main after
    the predicted move), so :meth:`validate_against` can prove the prepared
    move is still legal before the runner commits it.
    """

    schema: int
    predicted_move: str
    prepared_move: str
    position_fen: str

    @classmethod
    def from_dict(cls, data: dict) -> "PreparedResponse":
        if data.get("schema") != 1:
            raise StateError("unsupported prepared-response schema")
        try:
            return cls(1, str(data["predicted_move"]),
                       str(data["prepared_move"]), str(data["position_fen"]))
        except (KeyError, TypeError) as exc:
            raise StateError(f"invalid prepared response: {exc}") from exc

    @classmethod
    def load(cls, path: Path) -> "PreparedResponse":
        try:
            return cls.from_dict(json.loads(Path(path).read_text()))
        except (KeyError, TypeError, json.JSONDecodeError) as exc:
            raise StateError(f"invalid prepared response: {exc}") from exc

    def to_dict(self) -> dict:
        return {"schema": self.schema, "predicted_move": self.predicted_move,
                "prepared_move": self.prepared_move,
                "position_fen": self.position_fen}

    def save(self, path: Path) -> None:
        """Atomic write, mirroring :meth:`GameState.save`."""
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp",
                                    dir=path.parent)
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(self.to_dict(), stream, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(name, path)
        finally:
            try:
                os.unlink(name)
            except FileNotFoundError:
                pass

    def validate_against(self, state: GameState) -> None:
        """Raise StateError unless this response matches and is legal in state."""
        if self.schema != 1 or self.position_fen != state.fen:
            raise StateError("prepared response position mismatch")
        if self.prepared_move not in state.position().legal_moves:
            raise StateError("prepared response is illegal")


@dataclass(frozen=True)
class CandidateResult:
    """One candidate worker's outcome: the prepared reply + bookkeeping."""

    predicted_move: str
    prepared_move: str
    position_fen: str
    latency_s: float
    tokens_in: int = 0
    tokens_out: int = 0


def run_candidate(root: Path, predicted_move: str, actor: Actor) -> CandidateResult:
    """Compute and persist the prepared response for one predicted move.

    Loads ``root/game.json`` (the speculator's side just moved), applies
    ``predicted_move`` so the opponent is now to move, asks ``actor`` for the
    opponent's reply on that position, validates the reply is legal, then
    atomically saves the predicted state and the prepared response.

    Only ``predicted_move`` is appended to ``game.json`` -- the reply is NOT
    applied here; the runner applies it via ``PreparedResponse.validate_against``
    once the prediction is confirmed. Nothing is written if validation fails.
    """
    root = Path(root)
    state = GameState.load(root / "game.json")
    new_state = state.apply(predicted_move)  # StateError if prediction illegal
    reply = actor.choose_move(new_state.position())
    if reply.move not in new_state.position().legal_moves:
        # Mirrors GameState.apply legality: an illegal reply is a hard failure.
        raise StateError(f"prepared reply is illegal: {reply.move}")
    # Validation passed; commit the predicted state and the staged reply.
    new_state.save(root / "game.json")
    prepared = PreparedResponse(schema=1, predicted_move=predicted_move,
                                prepared_move=reply.move,
                                position_fen=new_state.fen)
    prepared.save(root / "prepared_response.json")
    return CandidateResult(predicted_move=predicted_move,
                           prepared_move=reply.move,
                           position_fen=new_state.fen,
                           latency_s=reply.latency_s,
                           tokens_in=reply.tokens_in,
                           tokens_out=reply.tokens_out)


# --- candidate attempt: launch / collect / cleanup --------------------------


class CandidateAttempt:
    """Owns one candidate's process, session, cgroup, and branch.

    The four ``*_created`` / ``process_live`` / ``session_registered`` flags
    are cleared only after the matching teardown step succeeds (or the
    resource is confirmed gone), so :meth:`cleanup` can be retried by an outer
    handler for whatever subset of resources remains. The teardown order is
    fixed: ``kill`` (process group + cgroup drain) -> ``unregister`` (session)
    -> ``destroy`` (cgroup directory) -> ``delete`` (branch). On a failed
    step the later flags stay set for the next retry.
    """

    def __init__(self, *, avfs: AgentVFS, cgroup, branch: str,
                 session_id: int, mount: Path, worker_argv: list[str],
                 worker_env: dict, results_dir: Path, predicted_move: str,
                 step: int = -1):
        self.avfs = avfs
        self.cgroup = cgroup
        self.branch = branch
        self.session_id = session_id
        self.mount = Path(mount)
        self.worker_argv = list(worker_argv)
        self.worker_env = dict(worker_env)
        self.results_dir = Path(results_dir)
        self.predicted_move = predicted_move
        self.step = step
        self.process = None
        self._stderr_fh = None
        # Resource-ownership flags. Cleared only on successful teardown.
        self.process_live = False
        self.session_registered = False
        self.cgroup_created = False
        self.branch_created = False

    def launch(self) -> None:
        """Spawn the worker inside the cgroup with stdout piped and stderr to
        an off-mount per-candidate log. Resolved secrets never appear on argv:
        they ride in ``worker_env`` and are read by name inside the worker."""
        if self.process is not None:
            raise CandidateWorkError(f"attempt {self.branch} already launched")
        if self.cgroup is None:
            raise CandidateWorkError(f"attempt {self.branch} has no cgroup")
        argv = [*self.worker_argv,
                "--root", str(self.mount),
                "--predicted-move", self.predicted_move]
        self.results_dir.mkdir(parents=True, exist_ok=True)
        log_path = self.results_dir / f"candidate-{self.branch}.log"
        try:
            self._stderr_fh = open(log_path, "ab")
        except OSError as exc:
            raise CandidateWorkError(
                f"could not open candidate log {log_path}: {exc}") from exc
        try:
            self.process = self.cgroup.run(
                argv, cwd=str(self.mount), env=self.worker_env,
                stdout=subprocess.PIPE, stderr=self._stderr_fh)
        except Exception as exc:
            self._stderr_fh.close()
            self._stderr_fh = None
            raise CandidateWorkError(
                f"worker spawn failed for {self.branch}: "
                f"{type(exc).__name__}") from exc
        self.process_live = True

    def collect(self, timeout_s: float) -> CandidateResult:
        """Reap the worker and parse exactly one CandidateResult JSON line.

        Rejects nonzero exit, missing/multiple stdout records, invalid JSON,
        or a prediction mismatch, each as :class:`CandidateWorkError`. On a
        normal return the process has exited, so ``process_live`` is cleared
        (the kill step is then a no-op during cleanup).
        """
        if self.process is None:
            raise CandidateWorkError(
                f"collect before launch for {self.branch}")
        try:
            out, _err = self.process.communicate(timeout=timeout_s)
        except subprocess.TimeoutExpired as exc:
            # Still running: leave process_live set so cleanup kills it.
            raise CandidateWorkError(
                f"worker timeout after {timeout_s}s for {self.branch}") from exc
        self.process_live = False
        if self.process.returncode != 0:
            raise CandidateWorkError(
                f"worker {self.branch} exit rc={self.process.returncode}")
        text = out.decode() if isinstance(out, (bytes, bytearray)) else (out or "")
        records = [ln for ln in text.splitlines() if ln.strip()]
        if len(records) != 1:
            raise CandidateWorkError(
                f"expected 1 stdout record from {self.branch}, "
                f"got {len(records)}")
        try:
            data = json.loads(records[0])
            result = CandidateResult(**data)
        except (json.JSONDecodeError, TypeError, ValueError) as exc:
            raise CandidateWorkError(
                f"invalid result JSON from {self.branch}: {exc}") from exc
        if result.predicted_move != self.predicted_move:
            raise CandidateWorkError(
                f"prediction mismatch from {self.branch}: "
                f"worker={result.predicted_move} "
                f"expected={self.predicted_move}")
        return result

    def cleanup(self) -> bool:
        """Tear down owned resources in kill -> unregister -> destroy -> delete
        order. Returns True only when every flag is clear AND the cgroup path
        is absent. A failed step does not advance; later flags stay set so a
        retry finishes them. Idempotent: a second call after a clean teardown
        performs no work."""
        # Release the parent-side stderr handle first. The child holds its own
        # inherited dup, so closing here never cuts off worker diagnostics; it
        # only avoids leaking one fd per candidate across the run.
        if self._stderr_fh is not None:
            try:
                self._stderr_fh.close()
            except OSError:
                pass
            self._stderr_fh = None
        # 1. kill -- process-group kill + cgroup drain (combined in kill_all).
        if self.process_live:
            if self.cgroup is not None:
                try:
                    self.cgroup.kill_all()
                except Exception:
                    return self._all_clear()  # failed step; do not advance
            # Reap the launched process so it is not left as a zombie. An
            # unreaped child stays listed in cgroup.procs, so destroy()'s
            # rmdir would fail (EBUSY); kill_all drains the cgroup but cannot
            # clear a zombie -- only the parent reaping can. On the hit path,
            # non-matching siblings are cleaned via require_cleanup without
            # ever being collected, so cleanup (not collect) must reap. Bound
            # the wait so a wedged process cannot stall teardown.
            if self.process is not None:
                try:
                    self.process.wait(timeout=5)
                except Exception:
                    pass
            self.process_live = False
        # 2. unregister -- remove the session routing entry.
        if self.session_registered:
            try:
                self.avfs.session_unregister(self.cgroup.path, step=self.step)
            except Exception:
                return self._all_clear()  # failed step; do not advance
            self.session_registered = False
        # 3. destroy -- remove the cgroup directory (drains any survivors).
        if self.cgroup_created:
            if self.cgroup is not None and self.cgroup.destroy():
                self.cgroup_created = False
            else:
                return self._all_clear()  # destroy failed; do not advance
        # 4. delete -- remove the speculative branch.
        if self.branch_created:
            try:
                self.avfs.branch_delete(self.branch)
            except Exception:
                return self._all_clear()  # failed step; do not advance
            self.branch_created = False
        return self._all_clear()

    def _all_clear(self) -> bool:
        """True iff no flag remains set and the cgroup path is gone."""
        if (self.cgroup is not None
                and Path(self.cgroup.path).exists()):
            return False
        return not (self.process_live or self.session_registered
                    or self.cgroup_created or self.branch_created)


class CandidateFactory:
    """Builds unlaunched :class:`CandidateAttempt` instances.

    ``create`` allocates a uniquely-named branch, a cgroup, and a registered
    session, then returns an unlaunched attempt. On any exception it cleans
    partial state; if partial cleanup leaves a resource it raises
    :class:`CleanupError` chained from the original exception. AgentVFS control
    failures propagate as :class:`CtlError`; cgroup-construction and
    process-spawn failures become :class:`CandidateWorkError`.
    """

    def __init__(self, *, avfs: AgentVFS, mount: Path, results_dir: Path,
                 cgroup_factory: Callable[[str], CgroupSession],
                 worker_argv: list[str], worker_env: dict):
        self.avfs = avfs
        self.mount = Path(mount)
        self.results_dir = Path(results_dir)
        self.cgroup_factory = cgroup_factory
        self.worker_argv = list(worker_argv)
        self.worker_env = dict(worker_env)

    def create(self, *, step: int, index: int,
               predicted_move: str) -> CandidateAttempt:
        branch = f"chess-spec-{step}-{index}"
        session_id = 10_000 + step * 64 + index
        cgroup_name = f"chess-cand-{step}-{index}"
        attempt = CandidateAttempt(
            avfs=self.avfs, cgroup=None, branch=branch, session_id=session_id,
            mount=self.mount, worker_argv=self.worker_argv,
            worker_env=self.worker_env, results_dir=self.results_dir,
            predicted_move=predicted_move, step=step)
        try:
            self.avfs.branch_create(branch, step=step)
            attempt.branch_created = True
            try:
                attempt.cgroup = self.cgroup_factory(cgroup_name)
            except Exception as exc:
                raise CandidateWorkError(
                    f"cgroup construction failed for {branch}: "
                    f"{type(exc).__name__}") from exc
            attempt.cgroup_created = True
            self.avfs.session_register(
                attempt.cgroup.path, session_id, branch, step=step)
            attempt.session_registered = True
        except CtlError:
            # Control failure: clean partial, then propagate (CtlError) or, if
            # resources remain, raise CleanupError chained from this CtlError.
            self._abort(attempt)
            raise
        except CandidateWorkError:
            self._abort(attempt)
            raise
        except Exception as exc:
            # Unexpected failure: clean partial, then surface as work error.
            self._abort(attempt)
            raise CandidateWorkError(
                f"candidate create failed for {branch}: {exc}") from exc
        return attempt

    def _abort(self, attempt: CandidateAttempt) -> None:
        """Best-effort cleanup of a partially-built attempt. If resources
        remain, raise :class:`CleanupError` chained from the in-flight
        exception (obtained via ``sys.exc_info`` so the original is preserved
        on the chain even when we raise here)."""
        if not attempt.cleanup():
            in_flight = sys.exc_info()[1]
            raise CleanupError(
                f"resources remain after create cleanup for {attempt.branch}"
            ) from in_flight


# --- worker CLI -------------------------------------------------------------
# Resolves endpoint and secret from NAMED environment variables, emits exactly
# one serialized CandidateResult line on stdout, and reserves stderr for
# diagnostics. Endpoint values, secrets, headers, and the exception object
# never appear on stdout/stderr; on malformed opposing-Actor output only the
# bounded raw_excerpt is written to the off-mount stderr log.


def _build_actor(args) -> Actor:
    base_url = os.environ.get(args.base_url_env) or None
    api_key = os.environ.get(args.api_key_env) or None
    try:
        import openai
        client = openai.OpenAI(base_url=base_url, api_key=api_key)
    except Exception:
        # Never surface the exception (it may carry client configuration).
        raise _ClientConstructionFailed()
    return OpenAIActor(client=client, model=args.model,
                       timeout_s=args.timeout_s, retries=args.retries)


class _ClientConstructionFailed(Exception):
    """Internal signal: client construction failed; the type name (only) is
    safe to log."""


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="candidate_worker", description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--predicted-move", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--base-url-env", required=True,
                        help="name of the env var holding the endpoint URL")
    parser.add_argument("--api-key-env", required=True,
                        help="name of the env var holding the API key")
    parser.add_argument("--timeout-s", type=float, default=30.0)
    parser.add_argument("--retries", type=int, default=2)
    args = parser.parse_args(argv)

    try:
        actor = _build_actor(args)
    except _ClientConstructionFailed:
        sys.stderr.write(
            "candidate worker: OpenAI client construction failed; "
            "see configured endpoint and credentials\n")
        return 2

    try:
        result = run_candidate(Path(args.root), args.predicted_move, actor)
    except LLMOutputError as exc:
        # Only the bounded model-text excerpt is safe to log; the exception
        # object, endpoint, and headers are never written.
        sys.stderr.write(exc.raw_excerpt)
        sys.stderr.flush()
        return 1
    except StateError as exc:
        sys.stderr.write(f"candidate worker: state error: {exc}\n")
        return 3

    sys.stdout.write(json.dumps(asdict(result)) + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised by the runner
    sys.exit(main())
