"""cgroup v2 session: create a cgroup, run processes inside it, kill, destroy.

The child enrolls ITSELF into the cgroup before exec so every write the
worker makes goes through the cgroup -- this single enrollment line is the
entire branch-routing mechanism for the speculative-chess benchmark.

Placement order (first writable wins):
1. ``$CHESS_CGROUP_ROOT/<name>-<pid>`` if that env var is set (explicit
   override, e.g. a pre-provisioned delegation);
2. ``/sys/fs/cgroup/<name>-<pid>`` (root in a writable cgroup v2 hierarchy);
3. a child of the current process's own delegated cgroup
   (``/proc/self/cgroup``), as systemd user delegation provides.

If none is writable, :class:`CgroupUnavailable` is raised -- a skippable
platform prerequisite, not a test failure. Ported from
``speculative-ml-research`` (with the chess env-var name and an idempotent
``destroy() -> bool``) without importing it.
"""

import os
import signal
import subprocess
import time
from pathlib import Path

CGROUP_FS = Path("/sys/fs/cgroup")

# The child enrolls ITSELF before exec so every write goes through the cgroup.
# run() and run_text() must always agree on it: $0 is the cgroup path (bash
# positional, set because we pass self.path as argv[3]); $$ is the bash pid
# that will become the worker after `exec "$@"`.
_ENROLL = 'echo $$ > "$0/cgroup.procs" && exec "$@"'


class CgroupUnavailable(RuntimeError):
    """No writable cgroup root was found; the caller should skip cgroup
    -dependent work rather than treat this as a hard failure."""


def _candidates(name: str) -> list[Path]:
    """Yield writable cgroup parent candidates in priority order.

    Each candidate is a unique per-pid directory under one of the three
    roots; the first one :meth:`CgroupSession.__init__` can mkdir and read
    ``cgroup.procs`` from wins."""
    uniq = f"{name}-{os.getpid()}"
    out: list[Path] = []
    if os.environ.get("CHESS_CGROUP_ROOT"):
        out.append(Path(os.environ["CHESS_CGROUP_ROOT"]) / uniq)
    out.append(CGROUP_FS / uniq)
    try:
        rel = (Path("/proc/self/cgroup").read_text().strip()
               .split("::", 1)[1].lstrip("/"))
        out.append(CGROUP_FS / rel / uniq)
    except (OSError, IndexError):
        pass
    return out


class CgroupSession:
    """A cgroup v2 leaf directory we own, with helpers to run, reap, and
    remove it."""

    def __init__(self, name: str):
        self.children: list[subprocess.Popen] = []
        self.path: str = ""
        errors: list[str] = []
        for cand in _candidates(name):
            try:
                cand.mkdir(parents=True, exist_ok=False)
                (cand / "cgroup.procs").read_text()  # readable => usable
                self.path = str(cand)
                return
            except OSError as e:
                errors.append(f"{cand}: {e}")
        raise CgroupUnavailable("; ".join(errors))

    def run(self, argv, cwd, env=None, stdout=None, stderr=None):
        """Spawn ``argv`` inside this cgroup via the self-enrolling bash
        wrapper. The child writes its own pid to ``cgroup.procs`` before
        exec, so the enrollment is race-free regardless of when the kernel
        first observes the process."""
        process = subprocess.Popen(
            ["bash", "-c", _ENROLL, self.path, *map(str, argv)],
            cwd=str(cwd), env={**os.environ, **(env or {})},
            stdout=stdout, stderr=stderr, start_new_session=True)
        self.children.append(process)
        return process

    def run_text(self, argv, cwd, timeout=60, input_text=None) -> str:
        """Run ``argv`` in the cgroup, block, and return captured stdout.

        Raises :class:`RuntimeError` on non-zero exit so callers can tell a
        worker crash from a missing cgroup."""
        r = subprocess.run(
            ["bash", "-c", _ENROLL, self.path, *map(str, argv)],
            cwd=str(cwd), capture_output=True, text=True,
            timeout=timeout, input=input_text)
        if r.returncode != 0:
            raise RuntimeError(f"cgroup run {argv} rc={r.returncode}: {r.stderr}")
        return r.stdout

    def kill_all(self) -> None:
        """Kill everything in this cgroup, robustly.

        Order matters: SIGKILL every child PROCESS GROUP first (a child that
        has not finished enrolling into ``cgroup.procs`` yet would survive a
        cgroup-only kill, because ``run`` returns before the enroll write
        lands), then repeatedly issue ``cgroup.kill`` (or SIGKILL each
        listed pid on kernels without it) until ``cgroup.procs`` is empty or
        ten seconds elapse."""
        for p in self.children:
            try:
                # start_new_session => the child's pgid == its pid.
                os.killpg(p.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError, OSError):
                pass
        kill_file = Path(self.path) / "cgroup.kill"
        procs = Path(self.path) / "cgroup.procs"
        try:
            deadline = time.time() + 10
            delay = 0.002  # SIGKILLed processes reap in ms; back off to 100ms
            while True:
                if kill_file.exists():
                    kill_file.write_text("1")
                else:
                    for pid in procs.read_text().split():
                        try:
                            os.kill(int(pid), signal.SIGKILL)
                        except (ProcessLookupError, ValueError):
                            pass
                if not procs.read_text().strip():
                    return
                if time.time() >= deadline:
                    break
                time.sleep(delay)
                delay = min(delay * 2, 0.1)
        except FileNotFoundError:
            return  # cgroup dir already removed => nothing left running

    def destroy(self) -> bool:
        """Tear the cgroup down completely. Returns True only when the path
        no longer exists.

        ``kill_all`` then ``os.rmdir``; repeated calls on an absent path
        return True; a remaining directory returns False -- a cleanup
        FAILURE, not a warning -- so the caller can fail loudly."""
        self.kill_all()
        try:
            os.rmdir(self.path)
        except OSError:
            pass
        return not Path(self.path).exists()
