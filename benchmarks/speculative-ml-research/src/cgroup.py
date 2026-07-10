"""cgroup v2 session: create a cgroup, run processes inside it, kill, destroy.

Placement order:
1. $SML_CGROUP_ROOT/<name>-<pid> if that env var is set (explicit override);
2. /sys/fs/cgroup/<name>-<pid> (root);
3. a child of the current process's own cgroup (systemd user delegation).
"""
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

CGROUP_FS = Path("/sys/fs/cgroup")

# The child enrolls ITSELF before exec so every write goes through the cgroup.
# This one line is the entire branch-routing mechanism — run() and run_text()
# must always agree on it.
_ENROLL = 'echo $$ > "$0/cgroup.procs" && exec "$@"'


class CgroupUnavailable(RuntimeError):
    pass


def _candidates(name: str) -> list[Path]:
    uniq = f"{name}-{os.getpid()}"
    out = []
    if os.environ.get("SML_CGROUP_ROOT"):
        out.append(Path(os.environ["SML_CGROUP_ROOT"]) / uniq)
    out.append(CGROUP_FS / uniq)
    try:
        rel = Path("/proc/self/cgroup").read_text().strip().split("::", 1)[1].lstrip("/")
        out.append(CGROUP_FS / rel / uniq)
    except (OSError, IndexError):
        pass
    return out


class CgroupSession:
    def __init__(self, name: str):
        self.children: list[subprocess.Popen] = []
        errors = []
        for cand in _candidates(name):
            try:
                cand.mkdir(parents=True, exist_ok=False)
                (cand / "cgroup.procs").read_text()  # readable => usable
                self.path = str(cand)
                return
            except OSError as e:
                errors.append(f"{cand}: {e}")
        raise CgroupUnavailable("; ".join(errors))

    def run(self, argv, cwd, env=None, stdout=None) -> subprocess.Popen:
        proc = subprocess.Popen(
            ["bash", "-c", _ENROLL, self.path, *argv],
            cwd=str(cwd), env={**os.environ, **(env or {})},
            stdout=stdout, stderr=subprocess.STDOUT if stdout else None,
            start_new_session=True,
        )
        self.children.append(proc)
        return proc

    def run_text(self, argv, cwd, timeout=60, input_text=None) -> str:
        r = subprocess.run(["bash", "-c", _ENROLL, self.path, *argv],
                           cwd=str(cwd), capture_output=True, text=True,
                           timeout=timeout, input=input_text)
        if r.returncode != 0:
            raise RuntimeError(f"cgroup run {argv} rc={r.returncode}: {r.stderr}")
        return r.stdout

    def kill_all(self) -> None:
        # Kill the process groups we spawned FIRST: a child that hasn't
        # finished enrolling into cgroup.procs yet would survive a
        # cgroup-only kill (run() returns before the enroll write lands).
        for p in self.children:
            try:
                os.killpg(p.pid, signal.SIGKILL)  # start_new_session => pgid == pid
            except ProcessLookupError:
                pass
        # Then kill everything enrolled, re-issuing inside the drain loop to
        # catch processes that finish enrolling after a kill round.
        kill_file = Path(self.path) / "cgroup.kill"
        procs = Path(self.path) / "cgroup.procs"
        try:
            deadline = time.time() + 10
            delay = 0.002  # SIGKILLed processes reap in ms; back off toward 100ms
            while True:
                if kill_file.exists():
                    kill_file.write_text("1")
                else:
                    for pid in procs.read_text().split():
                        try:
                            os.kill(int(pid), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                if not procs.read_text().strip():
                    return
                if time.time() >= deadline:
                    break
                time.sleep(delay)
                delay = min(delay * 2, 0.1)
        except FileNotFoundError:
            return  # cgroup dir already removed => nothing left running
        print(f"WARN: processes still in {self.path} 10s after SIGKILL",
              file=sys.stderr)

    def destroy(self) -> None:
        self.kill_all()
        try:
            os.rmdir(self.path)
        except OSError:
            pass
