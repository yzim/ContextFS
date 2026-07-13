"""Narrow subprocess wrapper over ``agentvfs-ctl`` for the speculative-chess
benchmark, plus daemon-lifecycle helpers shared by the CLI and fixtures.

This mirrors the wire format of ``benchmarks/speculative-ml-research`` (the
``--sock``, ``--json branch list``, ``branch create NAME --from main``,
``branch merge SOURCE --into main`` and ``session register ...`` argument
shapes; the readiness condition of a reachable control socket plus a seeded
mount sentinel file; the ``fusermount3`` teardown) WITHOUT importing that
package. Every ctl verb is funneled through :meth:`AgentVFS._ctl`, which
records the outcome (latency + status) on a verb journal *before* raising,
so a failed speculation is always attributable.
"""

import json
import os
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path


class CtlError(RuntimeError):
    """Raised when ``agentvfs-ctl`` exits non-zero."""


@dataclass
class VerbRecord:
    """One observed control verb: name, harness step, latency, outcome, and
    the CAS store size sampled for store-growth verbs (checkpoint/merge)."""
    verb: str
    step: int
    latency_us: int
    status: str
    store_bytes_after: int


# Only checkpoint and merge add objects to the CAS, so only those verbs
# re-sample the store size: `du` walks the whole object store, and branch
# create/delete run inside the speculation window, which must not pay an
# O(store) walk. peak_store_bytes stays exact -- the store peaks right after
# a checkpoint or merge. (Mirrors speculative-ml-research's growth set.)


@dataclass
class AgentVFS:
    """Subprocess wrapper around ``agentvfs-ctl``.

    ``sock`` is the daemon control socket path; ``store_objects`` is the CAS
    directory measured by :meth:`store_bytes`; ``ctl_bin`` is the ctl
    executable. ``verbs`` is the per-step verb journal consumed by the
    metrics layer, and ``active_sessions`` tracks cgroup paths currently
    registered with the daemon (added after a successful register, removed
    after a successful unregister).
    """
    sock: str
    store_objects: Path
    ctl_bin: Path
    verbs: list = field(default_factory=list)
    active_sessions: set = field(default_factory=set)
    _last_store_bytes: int = 0

    def _ctl(self, verb: str, args: list[str], step: int = -1,
             record: bool = True) -> str:
        command = [str(self.ctl_bin), "--sock", self.sock, *args]
        started = time.perf_counter_ns()
        result = subprocess.run(command, capture_output=True, text=True)
        latency_us = (time.perf_counter_ns() - started) // 1000
        status = "ok" if result.returncode == 0 else "error"
        if record:
            if verb in {"checkpoint", "merge"}:
                self._last_store_bytes = self.store_bytes()
            self.verbs.append(VerbRecord(verb, step, latency_us, status,
                                         self._last_store_bytes))
        if result.returncode:
            raise CtlError(f"{' '.join(command)} failed: "
                           f"{result.stdout.strip()} {result.stderr.strip()}")
        return result.stdout.strip()

    def store_bytes(self) -> int:
        """Total bytes in the CAS object store via ``du -sb``.

        Returns 0 if ``du`` fails or the store is not yet populated; never
        raises so a failed measurement cannot abort a checkpoint journal."""
        r = subprocess.run(["du", "-sb", str(self.store_objects)],
                           capture_output=True, text=True)
        return int(r.stdout.split()[0]) if r.returncode == 0 and r.stdout else 0

    # --- branch / checkpoint verbs ------------------------------------------
    def checkpoint(self, label: str, step: int = -1,
                   branch: str | None = None) -> str:
        args = ["checkpoint", label] + (["--branch", branch] if branch else [])
        return self._ctl("checkpoint", args, step)

    def branch_create(self, name: str, step: int = -1) -> str:
        return self._ctl("branch_create",
                         ["branch", "create", name, "--from", "main"], step)

    def branch_delete(self, name: str, step: int = -1) -> None:
        self._ctl("branch_delete", ["branch", "delete", name], step)

    def branch_merge(self, source: str, into: str = "main",
                     step: int = -1) -> str:
        return self._ctl("merge", ["branch", "merge", source, "--into", into],
                         step)

    def branch_list(self) -> list:
        out = self._ctl("branch_list", ["--json", "branch", "list"], record=False)
        data = json.loads(out)
        return data if isinstance(data, list) else data.get("branches", [])

    def status(self) -> dict:
        return json.loads(self._ctl("status", ["--json", "status"], record=False))

    # --- session routing ----------------------------------------------------
    # active_sessions is mutated only on ctl success (the raise in _ctl guards
    # the add/discard): add a cgroup path after a successful register, remove
    # it only after a successful unregister.
    def session_register(self, cgroup_path: str, session_id: int,
                         branch: str, step: int = -1) -> None:
        self._ctl("session_register",
                  ["session", "register", "--cgroup", cgroup_path,
                   "--id", str(session_id), "--branch", branch], step=step)
        self.active_sessions.add(cgroup_path)

    def session_unregister(self, cgroup_path: str, step: int = -1) -> None:
        self._ctl("session_unregister",
                  ["session", "unregister", "--cgroup", cgroup_path],
                  step=step)
        self.active_sessions.discard(cgroup_path)


# --- daemon lifecycle, shared by the CLI and the Task 7 real fixture --------
# The argv contract and the readiness signal (control socket reachable + a
# seeded sentinel file visible through the mount) live here so the fixture
# exercises the same protocol the entrypoint uses. Ported from
# speculative-ml-research without importing it.

def daemon_argv(bin_path, source, mountpoint, store, sock) -> list:
    """Argv to launch the agentvfs daemon in foreground mode.

    Mirrors speculative-ml-research: ``--control-sock`` is the daemon-side
    socket flag (the client ``agentvfs-ctl`` speaks to it via ``--sock``)."""
    return [str(bin_path), "--source", str(source),
            "--mountpoint", str(mountpoint), "--store", str(store),
            "--control-sock", str(sock), "-f"]


def wait_daemon_ready(sock, sentinel: Path, tries: int = 200,
                      interval: float = 0.05) -> bool:
    """Ready when the control socket is reachable AND the seeded mount
    sentinel file is readable. Returns False after ``tries`` without ready."""
    for _ in range(tries):
        if Path(sock).is_socket() and Path(sentinel).exists():
            return True
        time.sleep(interval)
    return False


def _is_mount(path) -> bool:
    """True iff ``path`` is currently a mount point (FUSE or otherwise).

    ``os.path.ismount`` resolves a real stat() on the path, so a stale FUSE
    mount reads as mounted until ``fusermount3 -u`` succeeds."""
    try:
        return os.path.ismount(str(path))
    except OSError:
        return False


def stop_daemon(proc, mount) -> str | None:
    """Idempotent daemon teardown. Never raises.

    Tolerates partial startup (``proc`` may be None / never started):
    terminates and reaps the daemon with bounded waits, attempts
    ``fusermount3 -u`` only when ``mount`` is actually mounted, and returns
    one diagnostic string (or None on full success) so the caller can log
    incomplete cleanup without trapping exceptions.
    """
    diag: list[str] = []
    if proc is not None:
        if proc.poll() is None:  # still running
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    diag.append(
                        f"daemon pid={getattr(proc, 'pid', '?')} "
                        f"unresponsive after SIGKILL")
            except OSError as e:
                diag.append(f"daemon terminate error: {e}")
    # Unmount only when the path is actually a mount point; calling
    # fusermount3 on an absent/stale path errors noisily.
    if _is_mount(mount):
        try:
            r = subprocess.run(["fusermount3", "-u", str(mount)],
                               capture_output=True, text=True, timeout=10)
            if r.returncode != 0:
                diag.append(
                    f"fusermount3 -u {mount} failed rc={r.returncode}: "
                    f"{r.stderr.strip()}")
        except (OSError, subprocess.TimeoutExpired) as e:
            diag.append(f"unmount error: {e}")
    return "; ".join(diag) if diag else None
