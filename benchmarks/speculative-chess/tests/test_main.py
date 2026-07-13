"""Offline CLI / config / preflight contract tests for Task 7.

Every test here is fully offline: no daemon, no FUSE mount, no cgroup, no
network, and no model key. ``load_config`` secret resolution is driven by a
function-scoped autouse fixture that sets four dummy credentials, so the
contract tests can ``delenv`` the one under test and still have the rest of the
baseline present. ``preflight`` is exercised through local AgentVFS / cgroup
fakes that record branch / session / cgroup state, so the parameterized
cleanup tests run without any real infrastructure.
"""

from pathlib import Path
from types import SimpleNamespace

import pytest

import main as chess_main

# main.py is implemented in Step 2; importing here drives the RED step.
from main import (ConfigError, PreflightError, load_config, preflight)

from src.agentvfs import CtlError
from src.candidate_worker import CleanupError
from src.game import StateError


# --- baseline credentials --------------------------------------------------
# Four dummy credentials are set for every test in this module so a contract
# test can delete exactly the one variable under test (e.g. CHESS_SPEC_API_KEY)
# and observe load_config's per-mode requirement. Function-scoped monkeypatch
# restores the real environment after each test.
@pytest.fixture(autouse=True)
def _dummy_credentials(monkeypatch):
    monkeypatch.setenv("CHESS_ACTOR_API_KEY", "fake-actor-key")
    monkeypatch.setenv("CHESS_ACTOR_BASE_URL", "http://actor.example")
    monkeypatch.setenv("CHESS_SPEC_API_KEY", "fake-spec-key")
    monkeypatch.setenv("CHESS_SPEC_BASE_URL", "http://spec.example")


# The fixed YAML shape from the brief, with only ``k`` / ``k_launch`` varied.
def write_config(tmp_path, *, k, k_launch):
    path = tmp_path / "config.yml"
    path.write_text(
        f"""
max_plies: 50
k: {k}
k_launch: {k_launch}
candidate_timeout_s: 120
actor:
  model: "gpt-5"
  base_url_env: "CHESS_ACTOR_BASE_URL"
  api_key_env: "CHESS_ACTOR_API_KEY"
  timeout_s: 120
  retries: 3
speculator:
  model: "gpt-5-mini"
  base_url_env: "CHESS_SPEC_BASE_URL"
  api_key_env: "CHESS_SPEC_API_KEY"
  timeout_s: 30
  retries: 2
paths:
  agentvfs_bin: "../../build/agentvfs"
  ctl_bin: "../../build/agentvfs-ctl"
""".lstrip())
    return path


# --- Step 1: config / credential contract tests ----------------------------


def test_spec_config_requires_named_environment_variables(tmp_path, monkeypatch):
    path = write_config(tmp_path, k=3, k_launch=2)
    monkeypatch.delenv("CHESS_SPEC_API_KEY", raising=False)
    with pytest.raises(ConfigError, match="CHESS_SPEC_API_KEY"):
        load_config(path, "spec")


def test_config_rejects_launch_width_greater_than_prediction_width(tmp_path):
    path = write_config(tmp_path, k=2, k_launch=3)
    with pytest.raises(ConfigError, match="k_launch must be <= k"):
        load_config(path, "spec")


def test_regular_native_requires_only_actor_credentials(tmp_path, monkeypatch):
    """``regular-native`` must NOT require Speculator credentials: deleting the
    Speculator API key still loads successfully (no AgentVFS, actor-only)."""
    path = write_config(tmp_path, k=3, k_launch=2)
    monkeypatch.delenv("CHESS_SPEC_API_KEY", raising=False)
    monkeypatch.delenv("CHESS_SPEC_BASE_URL", raising=False)
    cfg = load_config(path, "regular-native")
    assert cfg.mode == "regular-native"


def test_regular_requires_only_actor_credentials(tmp_path, monkeypatch):
    """``regular`` likewise needs only Actor credentials (it launches AgentVFS
    but never consults the Speculator)."""
    path = write_config(tmp_path, k=3, k_launch=2)
    monkeypatch.delenv("CHESS_SPEC_API_KEY", raising=False)
    cfg = load_config(path, "regular")
    assert cfg.mode == "regular"


def test_actor_api_key_required_for_regular_native(tmp_path, monkeypatch):
    path = write_config(tmp_path, k=3, k_launch=2)
    monkeypatch.delenv("CHESS_ACTOR_API_KEY", raising=False)
    with pytest.raises(ConfigError, match="CHESS_ACTOR_API_KEY"):
        load_config(path, "regular-native")


def test_secrets_resolved_from_named_environment_variables(tmp_path):
    """Resolved secret values land on the returned config; the YAML carries only
    the variable NAMES, never the values."""
    path = write_config(tmp_path, k=3, k_launch=2)
    cfg = load_config(path, "spec")
    assert cfg.actor.api_key == "fake-actor-key"
    assert cfg.actor.base_url == "http://actor.example"
    assert cfg.speculator.api_key == "fake-spec-key"
    assert cfg.speculator.base_url == "http://spec.example"
    # The YAML never stored the secret, only the name of the variable.
    text = Path(path).read_text()
    assert "fake-actor-key" not in text
    assert "fake-spec-key" not in text


def test_config_carries_run_and_path_fields(tmp_path):
    """The resolved config surfaces the RunConfig inputs and the resolved bin
    paths (relative to the config file's directory)."""
    path = write_config(tmp_path, k=4, k_launch=2)
    cfg = load_config(path, "regular-native")
    assert cfg.max_plies == 50
    assert cfg.k == 4
    assert cfg.k_launch == 2
    assert cfg.candidate_timeout_s == 120
    # Paths resolve relative to the config file's directory (../../build/...),
    # matching the repo-relative convention in config.yml.
    assert cfg.paths.agentvfs_bin == (path.parent / "../../build/agentvfs").resolve()
    assert cfg.paths.ctl_bin == (path.parent / "../../build/agentvfs-ctl").resolve()


def test_unknown_mode_rejected(tmp_path):
    path = write_config(tmp_path, k=3, k_launch=2)
    with pytest.raises(ConfigError, match="mode"):
        load_config(path, "bogus")


def _runtime_config(mode):
    model = SimpleNamespace(model="m", base_url_env="BASE",
                            api_key_env="KEY", timeout_s=1, retries=0,
                            base_url=None, api_key="key")
    return SimpleNamespace(
        mode=mode, max_plies=1, k=1, k_launch=1,
        candidate_timeout_s=1, actor=model, speculator=model,
        paths=SimpleNamespace(agentvfs_bin=Path("agentvfs"),
                              ctl_bin=Path("agentvfs-ctl")))


def test_default_results_directory_is_unique_per_mode_and_timestamp(
        tmp_path, monkeypatch):
    captured = {}
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(chess_main, "load_config",
                        lambda path, mode: _runtime_config(mode))
    monkeypatch.setattr(chess_main, "_build_actor", lambda cfg: object())
    monkeypatch.setattr(chess_main.time, "strftime",
                        lambda fmt: "20260713-120000")

    def fake_run(root, actor, cfg, **kwargs):
        captured["results_dir"] = cfg.results_dir

    monkeypatch.setattr(chess_main, "run_regular", fake_run)

    assert chess_main.main([
        "--mode", "regular-native", "--config", "ignored.yml",
        "--run-dir", str(tmp_path / "work")]) == 0
    assert captured["results_dir"] == Path(
        "results/regular-native-20260713-120000")


def test_main_fails_when_daemon_cleanup_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(chess_main, "load_config",
                        lambda path, mode: _runtime_config(mode))
    monkeypatch.setattr(chess_main, "_build_actor", lambda cfg: object())
    monkeypatch.setattr(chess_main, "start_daemon",
                        lambda *args: (object(), SimpleNamespace()))
    monkeypatch.setattr(chess_main, "run_regular", lambda *args, **kwargs: None)
    monkeypatch.setattr(chess_main, "stop_daemon",
                        lambda proc, mount: "unmount failed")
    monkeypatch.setattr(chess_main.os.path, "ismount", lambda path: False)

    with pytest.raises(CleanupError, match="unmount failed"):
        chess_main.main([
            "--mode", "regular", "--config", "ignored.yml",
            "--run-dir", str(tmp_path / "run"),
            "--results-dir", str(tmp_path / "results")])


def test_main_preserves_body_error_and_chains_cleanup_failure(
        tmp_path, monkeypatch):
    monkeypatch.setattr(chess_main, "load_config",
                        lambda path, mode: _runtime_config(mode))
    monkeypatch.setattr(chess_main, "_build_actor", lambda cfg: object())
    monkeypatch.setattr(chess_main, "start_daemon",
                        lambda *args: (object(), SimpleNamespace()))

    def fail_run(*args, **kwargs):
        raise StateError("corrupt state")

    monkeypatch.setattr(chess_main, "run_regular", fail_run)
    monkeypatch.setattr(chess_main, "stop_daemon",
                        lambda proc, mount: "unmount failed")
    monkeypatch.setattr(chess_main.os.path, "ismount", lambda path: False)

    with pytest.raises(StateError, match="corrupt state") as exc_info:
        chess_main.main([
            "--mode", "regular", "--config", "ignored.yml",
            "--run-dir", str(tmp_path / "run"),
            "--results-dir", str(tmp_path / "results")])
    assert isinstance(exc_info.value.__cause__, CleanupError)


# --- preflight fakes + parameterized cleanup tests --------------------------


class _PreflightAVFS:
    """In-memory AgentVFS stand-in for preflight. Records the probe branch and
    session so a test can assert they were removed by cleanup."""

    def __init__(self):
        self.branches: set[str] = set()
        self.sessions: dict[str, str] = {}  # cgroup path -> branch

    def status(self):
        return {"ok": True}

    def branch_list(self):
        return [{"name": b} for b in sorted(self.branches)]

    def branch_create(self, name, step=-1):
        self.branches.add(name)

    def session_register(self, cgroup_path, session_id, branch, step=-1):
        self.sessions[cgroup_path] = branch

    def session_unregister(self, cgroup_path, step=-1):
        self.sessions.pop(cgroup_path, None)

    def branch_delete(self, name, step=-1):
        self.branches.discard(name)


class _PreflightCgroup:
    """Cgroup stand-in: records its path, optionally leaks the routed marker
    onto main (to trip the invisibility check), optionally fails the routed
    write, and remembers whether ``destroy`` ran."""

    def __init__(self, path, *, fail_write=False, leak_marker=None):
        self.path = str(path)
        self.fail_write = fail_write
        self.leak_marker = leak_marker  # Path written to main to simulate a leak
        self.destroyed = False

    def run_text(self, argv, cwd, **kwargs):
        if self.fail_write:
            raise RuntimeError("routed probe write failed (scripted)")
        if self.leak_marker is not None:
            Path(self.leak_marker).write_text("probe")
        return ""

    def destroy(self):
        self.destroyed = True
        return True


_PROBE_BRANCH = "chess-preflight-probe"


def test_preflight_happy_path_leaves_no_residue(tmp_path):
    """When isolation holds (the routed write is invisible on main), preflight
    completes without raising and removes the probe branch, session, and
    cgroup."""
    avfs = _PreflightAVFS()
    mount = tmp_path / "mnt"
    mount.mkdir()
    cg_dir = tmp_path / "cg"
    cg_dir.mkdir()
    cg = _PreflightCgroup(cg_dir)

    def factory(name):
        return cg

    preflight(avfs, mount, factory)  # must not raise

    assert _PROBE_BRANCH not in avfs.branches
    assert avfs.sessions == {}
    assert cg.destroyed is True


@pytest.mark.parametrize(
    "fail_point",
    ["after-branch", "after-register", "after-write"],
)
def test_preflight_cleans_up_when_a_step_fails(tmp_path, fail_point):
    """When preflight fails AFTER branch creation / registration / the probe
    write, the original error is raised and NO probe branch, session, or cgroup
    survives cleanup."""
    avfs = _PreflightAVFS()
    mount = tmp_path / "mnt"
    mount.mkdir()
    cg_dir = tmp_path / "cg"
    cg_dir.mkdir()
    marker = mount / ".preflight_marker"

    if fail_point == "after-branch":
        # The probe branch is created, then cgroup construction fails before
        # any session is registered.
        def factory(name):
            raise RuntimeError("probe cgroup construction failed")

        with pytest.raises(PreflightError):
            preflight(avfs, mount, factory)
    elif fail_point == "after-register":
        # branch + cgroup + session are all established, then the routed write
        # itself fails.
        cg = _PreflightCgroup(cg_dir, fail_write=True)

        def factory(name):
            return cg

        with pytest.raises(PreflightError):
            preflight(avfs, mount, factory)
        assert cg.destroyed is True
    else:  # after-write
        # The routed write "leaks" onto main -> isolation broken -> PreflightError.
        cg = _PreflightCgroup(cg_dir, leak_marker=marker)

        def factory(name):
            return cg

        with pytest.raises(PreflightError, match="isolation"):
            preflight(avfs, mount, factory)
        assert cg.destroyed is True

    # All three failure points must leave no probe branch and no live session.
    assert _PROBE_BRANCH not in avfs.branches
    assert avfs.sessions == {}


def test_preflight_chains_cleanup_error_when_cleanup_also_fails(tmp_path):
    """If the original preflight error occurs AND cleanup also fails, the
    CleanupError is chained on the raised PreflightError (not masked)."""
    avfs = _PreflightAVFS()
    mount = tmp_path / "mnt"
    mount.mkdir()
    cg_dir = tmp_path / "cg"
    cg_dir.mkdir()
    marker = mount / ".preflight_marker"
    cg = _PreflightCgroup(cg_dir, leak_marker=marker)

    # Sabotage cleanup: branch_delete fails so the two-pass cleanup cannot
    # remove the probe branch.
    def _branch_delete(name, step=-1):
        raise CtlError("branch delete failed (scripted)")

    avfs.branch_delete = _branch_delete

    def factory(name):
        return cg

    with pytest.raises(PreflightError) as exc_info:
        preflight(avfs, mount, factory)
    # The original isolation error is chained from a CleanupError.
    assert exc_info.value.__cause__ is not None
    assert "cleanup" in str(exc_info.value.__cause__).lower() or \
        "delete" in str(exc_info.value.__cause__).lower()
