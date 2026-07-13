"""CLI wiring for the speculative-chess benchmark (Task 7).

This module is the thin orchestration layer above the runner / worker /
AgentVFS infrastructure built in Tasks 1-6. It owns four things:

* :func:`load_config` -- parse the non-secret YAML, validate the mode and the
  ``k_launch <= k`` structural rule, and resolve model secrets from the NAMED
  environment variables (the YAML carries only the variable names, never the
  values). ``regular-native`` / ``regular`` require only Actor credentials;
  ``spec`` additionally requires Speculator credentials.
* :func:`start_daemon` / :func:`stop_daemon` use -- launch the AgentVFS daemon
  only for the ``regular`` and ``spec`` modes, and always reap it in ``finally``.
* :func:`preflight` -- spec-mode isolation probe: create a branch, route a probe
  cgroup write through it, prove the write is invisible on main, then clean up
  with the same ordered two-pass rule the candidate lifecycle uses. Fail closed
  (raise) before any model call if isolation is unavailable.
* :func:`main` -- the ``--mode`` / ``--config`` / ``--run-dir`` /
  ``--results-dir`` entrypoint. Results are created before any API work.
"""

import argparse
import os
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import yaml

from src.agentvfs import (AgentVFS, daemon_argv, stop_daemon, wait_daemon_ready)
from src.candidate_worker import CandidateFactory, CleanupError
from src.cgroup import CgroupSession
from src.game import GameState
from src.llm import OpenAIActor, OpenAISpeculator
from src.metrics import RunConfig
from src.runner_regular import run_regular
from src.runner_spec import run_spec
from src.verifier import Verifier

VALID_MODES = ("regular-native", "regular", "spec")


class ConfigError(ValueError):
    """Raised for malformed configuration, an unknown mode, an invalid fanout
    (``k_launch > k``), or an unresolvable named secret."""


class PreflightError(RuntimeError):
    """Raised when the spec-mode isolation preflight cannot prove a routed
    cgroup write is invisible on main (or when the probe setup failed)."""


# --- configuration model ---------------------------------------------------


@dataclass(frozen=True)
class ModelConfig:
    """One model endpoint. ``base_url`` / ``api_key`` are resolved from the
    named environment variables at load time; the YAML never stores values."""
    model: str
    base_url_env: str
    api_key_env: str
    timeout_s: float
    retries: int
    base_url: str | None
    api_key: str | None


@dataclass(frozen=True)
class PathsConfig:
    agentvfs_bin: Path
    ctl_bin: Path


@dataclass(frozen=True)
class Config:
    mode: str
    max_plies: int
    k: int
    k_launch: int
    candidate_timeout_s: float
    actor: ModelConfig
    speculator: ModelConfig
    paths: PathsConfig


def _env_or_none(name: str) -> str | None:
    """The named variable's value, or None when unset/empty. A base URL may
    legitimately be unset (OpenAI default endpoint), so it is optional."""
    return os.environ.get(name) or None


def _require_env(name: str) -> str:
    """The named variable's value; ConfigError when unset/empty. The API key is
    required, and the variable NAME is named in the message so an operator can
    see exactly which secret is missing."""
    value = os.environ.get(name)
    if not value:
        raise ConfigError(f"required environment variable {name} is not set")
    return value


def _model_section(raw: dict) -> tuple[str, str, str, float, int]:
    return (str(raw["model"]), str(raw["base_url_env"]),
            str(raw["api_key_env"]), float(raw["timeout_s"]),
            int(raw["retries"]))


def load_config(path, mode: str) -> Config:
    """Parse the non-secret YAML at ``path`` and resolve secrets for ``mode``.

    Structural validation precedes secret resolution: ``k_launch > k`` is
    rejected before any environment lookup. ``regular-native`` / ``regular``
    resolve and require only Actor credentials; ``spec`` additionally requires
    Speculator credentials. Bin paths resolve relative to the config file's
    directory (the YAML uses repo-relative paths like ``../../build/agentvfs``).
    """
    if mode not in VALID_MODES:
        raise ConfigError(
            f"unknown mode {mode!r}; expected one of {list(VALID_MODES)}")

    config_path = Path(path)
    try:
        raw = yaml.safe_load(config_path.read_text())
    except (OSError, yaml.YAMLError) as exc:
        raise ConfigError(f"could not read config {path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise ConfigError(f"config {path} is not a mapping")

    k = int(raw["k"])
    k_launch = int(raw["k_launch"])
    # Structural rule BEFORE any secret lookup, per the contract.
    if k_launch > k:
        raise ConfigError(
            f"k_launch must be <= k (k_launch={k_launch}, k={k})")

    a_model, a_base_env, a_key_env, a_to, a_ret = _model_section(raw["actor"])
    s_model, s_base_env, s_key_env, s_to, s_ret = _model_section(
        raw["speculator"])

    # Actor credentials are required for every mode (every mode drives an Actor).
    actor_base = _env_or_none(a_base_env)
    actor_key = _require_env(a_key_env)
    if mode == "spec":
        spec_base = _env_or_none(s_base_env)
        spec_key = _require_env(s_key_env)
    else:
        # regular-native / regular never consult the Speculator; its creds are
        # resolved best-effort (optional) so the returned Config is complete.
        spec_base = _env_or_none(s_base_env)
        spec_key = _env_or_none(s_key_env)

    actor = ModelConfig(a_model, a_base_env, a_key_env, a_to, a_ret,
                        actor_base, actor_key)
    speculator = ModelConfig(s_model, s_base_env, s_key_env, s_to, s_ret,
                             spec_base, spec_key)

    config_dir = config_path.parent
    paths = PathsConfig(
        agentvfs_bin=(config_dir / str(raw["paths"]["agentvfs_bin"])).resolve(),
        ctl_bin=(config_dir / str(raw["paths"]["ctl_bin"])).resolve())

    return Config(mode=mode, max_plies=int(raw["max_plies"]), k=k,
                  k_launch=k_launch,
                  candidate_timeout_s=float(raw["candidate_timeout_s"]),
                  actor=actor, speculator=speculator, paths=paths)


# --- daemon launch (regular / spec only) -----------------------------------


def start_daemon(agentvfs_bin, ctl_bin, source, mount, store, sock, log_path):
    """Launch the AgentVFS daemon in foreground mode and return ``(proc, avfs)``.

    Raises :class:`RuntimeError` if the daemon fails to launch or does not
    become ready (control socket reachable + the seeded mount sentinel readable)
    after a bounded wait. The caller MUST ``stop_daemon(proc, mount)`` in a
    ``finally``; this function never tears down on success.
    """
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    stream = log_path.open("wb")
    try:
        proc = subprocess.Popen(
            daemon_argv(agentvfs_bin, source, mount, store, sock),
            stdout=stream, stderr=subprocess.STDOUT)
    except Exception as exc:
        stream.close()
        raise RuntimeError(f"failed to launch agentvfs daemon: {exc}") from exc
    sentinel = Path(mount) / "game.json"
    if not wait_daemon_ready(sock, sentinel):
        stream.flush()
        tail = log_path.read_text(errors="replace")[-4096:]
        cleanup_error = stop_daemon(proc, mount)
        stream.close()
        if os.path.ismount(str(mount)) and not cleanup_error:
            cleanup_error = "mount remains after stop_daemon"
        startup_error = RuntimeError(
            f"agentvfs daemon did not become ready:\n{tail}")
        if cleanup_error:
            raise startup_error from CleanupError(cleanup_error)
        raise startup_error
    avfs = AgentVFS(sock=str(sock), store_objects=Path(store),
                    ctl_bin=Path(ctl_bin))
    return proc, avfs


# --- spec isolation preflight ----------------------------------------------


_PROBE_BRANCH = "chess-preflight-probe"
_PROBE_MARKER = ".preflight_marker"
_PROBE_SESSION_ID = 9999


def _preflight_cleanup(avfs, cgroup, *, branch_created, session_registered,
                       cgroup_created):
    """Ordered two-pass cleanup of the probe resources
    (unregister -> destroy -> delete). A failing step does not advance, mirroring
    :meth:`CandidateAttempt.cleanup`; the outer two-pass retry finishes whatever
    subset remains. Returns a diagnostic string (always containing "cleanup") if
    any resource survived or any step failed, else None. Never raises."""

    state = {"session": session_registered, "cgroup": cgroup_created,
             "branch": branch_created}
    fail_step = [None]  # list cell so the closure can rebind

    def one_pass() -> bool:
        # Stop at the first failing step within a single pass (the candidate
        # rule); the outer loop gives the second pass.
        if state["session"]:
            try:
                avfs.session_unregister(cgroup.path)
            except Exception:
                fail_step[0] = fail_step[0] or "session_unregister"
                return False
            state["session"] = False
        if state["cgroup"] and cgroup is not None:
            try:
                ok = cgroup.destroy()
            except Exception:
                fail_step[0] = fail_step[0] or "cgroup_destroy"
                return False
            if not ok:
                fail_step[0] = fail_step[0] or "cgroup_destroy"
                return False
            state["cgroup"] = False
        if state["branch"]:
            try:
                avfs.branch_delete(_PROBE_BRANCH)
            except Exception:
                fail_step[0] = fail_step[0] or "branch_delete"
                return False
            state["branch"] = False
        return True

    for _ in range(2):
        if one_pass():
            break

    leftover = [name for name, pending in state.items() if pending]
    if leftover or fail_step[0]:
        parts = []
        if fail_step[0]:
            parts.append(f"failed step: {fail_step[0]}")
        if leftover:
            parts.append(f"remaining: {leftover}")
        return "probe cleanup incomplete (" + "; ".join(parts) + ")"
    return None


def preflight(avfs, mount, cgroup_factory) -> None:
    """Spec-mode isolation preflight.

    Creates a probe branch, owns a probe cgroup, registers it routed to the
    probe branch, writes a marker THROUGH the probe cgroup (so the write lands
    on the probe branch), and proves the marker is INVISIBLE on main. Any
    failure becomes :class:`PreflightError`. Probe resources are torn down in a
    ``finally`` with the ordered two-pass rule; if cleanup also fails the
    :class:`CleanupError` is chained onto the raised error (original preserved).
    """
    mount = Path(mount)
    original_error = None
    probe_cgroup = None
    branch_created = False
    session_registered = False
    cgroup_created = False

    try:
        # 1. daemon JSON health (proves status()/branch_list() parse).
        avfs.status()
        avfs.branch_list()
        # 2. probe branch.
        avfs.branch_create(_PROBE_BRANCH)
        branch_created = True
        # 3. probe cgroup (owned by preflight).
        try:
            probe_cgroup = cgroup_factory("chess-preflight-cg")
        except Exception as exc:
            raise PreflightError(
                f"probe cgroup construction failed: {exc}") from exc
        cgroup_created = True
        # 4. register the probe session routed to the probe branch.
        avfs.session_register(probe_cgroup.path, _PROBE_SESSION_ID,
                              _PROBE_BRANCH)
        session_registered = True
        # 5. routed write through the probe cgroup (lands on the probe branch).
        marker = mount / _PROBE_MARKER
        probe_cgroup.run_text(
            ["sh", "-c", f"echo preflight > {shlex.quote(str(marker))}"],
            cwd=str(mount))
        # 6. prove the routed write is INVISIBLE on main.
        if marker.exists():
            raise PreflightError(
                "isolation unavailable: routed probe write is visible on main")
    except Exception as exc:
        original_error = exc
    finally:
        cleanup_diag = _preflight_cleanup(
            avfs, probe_cgroup, branch_created=branch_created,
            session_registered=session_registered, cgroup_created=cgroup_created)

    if original_error is not None:
        if cleanup_diag:
            raise PreflightError(str(original_error)) from CleanupError(cleanup_diag)
        if isinstance(original_error, PreflightError):
            raise original_error
        raise PreflightError(str(original_error)) from original_error
    if cleanup_diag:
        raise CleanupError(cleanup_diag)


# --- model / factory builders ----------------------------------------------


def _build_actor(mc: ModelConfig) -> OpenAIActor:
    import openai
    client = openai.OpenAI(base_url=mc.base_url, api_key=mc.api_key)
    return OpenAIActor(client=client, model=mc.model, timeout_s=mc.timeout_s,
                       retries=mc.retries)


def _build_speculator(mc: ModelConfig) -> OpenAISpeculator:
    import openai
    client = openai.OpenAI(base_url=mc.base_url, api_key=mc.api_key)
    return OpenAISpeculator(client=client, model=mc.model,
                            timeout_s=mc.timeout_s, retries=mc.retries)


def _candidate_factory(cfg: Config, avfs, mount, results_dir):
    """Build the real CandidateFactory with the worker command prefix that
    resolves ``src.candidate_worker``'s ``main`` via ``python -m``. Resolved
    secret values ride in ``worker_env`` under their named keys; the daemon
    side never receives them on argv. ``PYTHONPATH`` points at the project root
    so ``-m src.candidate_worker`` resolves from the worker's cwd (the mount)."""
    project_root = Path(__file__).resolve().parent
    worker_argv = [
        sys.executable, "-m", "src.candidate_worker",
        "--model", cfg.actor.model,
        "--base-url-env", cfg.actor.base_url_env,
        "--api-key-env", cfg.actor.api_key_env,
        "--timeout-s", str(cfg.actor.timeout_s),
        "--retries", str(cfg.actor.retries),
    ]
    worker_env = {
        "PYTHONPATH": str(project_root),
        cfg.actor.base_url_env: cfg.actor.base_url or "",
        cfg.actor.api_key_env: cfg.actor.api_key or "",
    }
    return CandidateFactory(
        avfs=avfs, mount=mount, results_dir=results_dir,
        cgroup_factory=lambda name: CgroupSession(name),
        worker_argv=worker_argv, worker_env=worker_env)


def _seed_game(source_dir: Path) -> None:
    """Seed the authoritative ``game.json`` at the start position if absent
    (resume semantics honor an existing file)."""
    source_dir.mkdir(parents=True, exist_ok=True)
    game_path = source_dir / "game.json"
    if not game_path.exists():
        GameState.initial().save(game_path)


# --- entrypoint ------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="speculative-chess",
        description="Speculative-chess AgentVFS benchmark CLI.")
    parser.add_argument("--mode", required=True, choices=list(VALID_MODES))
    parser.add_argument("--config", required=True,
                        help="path to the non-secret config.yml")
    parser.add_argument("--run-dir", default=None,
                        help="workspace directory for this run; when omitted a "
                             "fresh temporary working directory is created "
                             "(each invocation starts a new game)")
    parser.add_argument("--results-dir", default=None,
                        help="directory where results artifacts are written "
                             "(default: results/<mode>-<timestamp>)")
    args = parser.parse_args(argv)

    cfg = load_config(args.config, args.mode)
    # A None run-dir resolves to a fresh tempdir INSIDE main() (not as the
    # argparse default, which would create a dir even on --help). Each
    # invocation starts a new game; reusing a fixed workload dir would resume a
    # prior game's game.json. The tempdir is left in place after the run (the
    # committed trajectory is captured in results/committed.json + lossless.json;
    # the workload dir is ephemeral by design).
    if args.run_dir is None:
        run_dir = Path(tempfile.mkdtemp(prefix="chess-run-"))
    else:
        run_dir = Path(args.run_dir)
    if args.results_dir is None:
        run_id = f"{cfg.mode}-{time.strftime('%Y%m%d-%H%M%S')}"
        results_dir = Path("results") / run_id
    else:
        results_dir = Path(args.results_dir)

    # Results are created BEFORE any API work (the contract).
    results_dir.mkdir(parents=True, exist_ok=True)
    run_cfg = RunConfig(max_plies=cfg.max_plies, results_dir=results_dir,
                        k=cfg.k, k_launch=cfg.k_launch,
                        candidate_timeout_s=cfg.candidate_timeout_s)

    actor = _build_actor(cfg.actor)

    # regular-native: no AgentVFS, no verifier; a plain directory game.
    if cfg.mode == "regular-native":
        _seed_game(run_dir)
        run_regular(run_dir, actor, run_cfg, mode=cfg.mode)
        return 0

    # regular / spec: launch AgentVFS, operate through the mount, always reap.
    source = run_dir / "source"
    mount = run_dir / "mount"
    store = run_dir / "store"
    sock = str(run_dir / "control.sock")
    mount.mkdir(parents=True, exist_ok=True)
    _seed_game(source)

    proc = None
    body_error = None
    cleanup_error = None
    try:
        proc, avfs = start_daemon(
            cfg.paths.agentvfs_bin, cfg.paths.ctl_bin,
            source, mount, store, sock, run_dir / "daemon.log")
        verifier = Verifier(avfs, mount, results_dir)

        if cfg.mode == "regular":
            run_regular(mount, actor, run_cfg, avfs=avfs, verifier=verifier,
                        mode=cfg.mode)
        else:
            # spec: isolation preflight before any model call, then the runner.
            preflight(avfs, mount, lambda name: CgroupSession(name))
            speculator = _build_speculator(cfg.speculator)
            candidate_factory = _candidate_factory(
                cfg, avfs, mount, results_dir)
            run_spec(mount, actor, speculator, run_cfg, avfs, verifier,
                     candidate_factory)
    except BaseException as exc:
        body_error = exc
    finally:
        if proc is not None:
            cleanup_error = stop_daemon(proc, mount)
            if os.path.ismount(str(mount)) and not cleanup_error:
                cleanup_error = "mount remains after stop_daemon"

    if body_error is not None:
        if cleanup_error:
            raise body_error from CleanupError(cleanup_error)
        raise body_error
    if cleanup_error:
        raise CleanupError(cleanup_error)
    return 0


if __name__ == "__main__":  # pragma: no cover - operator entrypoint
    raise SystemExit(main())
