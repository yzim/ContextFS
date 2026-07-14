#!/usr/bin/env python3
"""Shared driver for the mem-gc benchmark scenarios.

Starts the agentvfs daemon on a synthetic tree, speaks the line-oriented
control protocol over the unix socket directly (no agentvfs-ctl dependency,
so old 'before' binaries work identically), and samples memory/store size.
Python 3.8+, stdlib only.
"""
import csv, json, os, shutil, socket, subprocess, time
from pathlib import Path

def gen_tree(src: Path, files: int, dirs: int, payload: bytes = b"x" * 64):
    """files spread evenly across dirs; deterministic names."""
    if files < 0 or dirs <= 0:
        raise ValueError("files must be non-negative and dirs must be positive")
    src.mkdir(parents=True, exist_ok=True)
    per = max(1, (files + dirs - 1) // dirs)
    n = 0
    for d in range(dirs):
        dp = src / f"dir{d:05d}"
        dp.mkdir(exist_ok=True)
        for f in range(per):
            if n >= files:
                return n
            (dp / f"f{f:05d}.txt").write_bytes(payload + str(n).encode())
            n += 1
    return n

class DaemonProc:
    def __init__(self, bin_path, src, mnt, store, sock):
        self.bin, self.src, self.mnt, self.store, self.sock = \
            map(str, (bin_path, src, mnt, store, sock))
        self.proc = None
    def start(self):
        Path(self.mnt).mkdir(parents=True, exist_ok=True)
        # NOTE: --control-sock (NOT --sock) is the real flag (src/cas/main.cpp:194)
        # and -f keeps the daemon in the foreground so Popen tracks the real
        # process and stop() can terminate it; without -f the daemon forks and
        # Popen would follow only the intermediate that exits.
        self.proc = subprocess.Popen(
            [self.bin, "--source", self.src, "--mountpoint", self.mnt,
             "--store", self.store, "--control-sock", self.sock, "-f"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        last_control_error = "control socket not present"
        for _ in range(300):
            rc = self.proc.poll()
            if rc is not None:
                raise RuntimeError(f"daemon exited during startup (status {rc})")
            control_ready = False
            if os.path.exists(self.sock):
                try:
                    response = self.ctl("status", timeout=0.5)
                    control_ready = response.get("ok") is True
                    if not control_ready:
                        last_control_error = f"status response was not ok: {response}"
                except Exception as exc:
                    last_control_error = str(exc)
            if control_ready and os.path.ismount(self.mnt):
                if self.proc.poll() is not None:
                    raise RuntimeError("daemon exited after startup readiness checks")
                return
            time.sleep(0.1)
        raise RuntimeError(
            "daemon did not expose both a responsive control socket and a mount "
            f"(mounted={os.path.ismount(self.mnt)}, control={last_control_error})")
    def ctl(self, line: str, timeout=120) -> dict:
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(timeout)
            s.connect(self.sock)
            s.sendall(line.encode() + b"\n")
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk: break
                buf += chunk
        return json.loads(buf.decode())
    def rss_kb(self) -> int:
        with open(f"/proc/{self.proc.pid}/status") as f:
            for ln in f:
                if ln.startswith("VmRSS:"): return int(ln.split()[1])
        return 0
    def force_full_ingest(self, expected_files: int, timeout=300.0, interval=0.1):
        """Walk until bootstrap exposes exactly the generated source tree."""
        deadline = time.monotonic() + timeout
        last_count = 0
        while True:
            rc = self.proc.poll() if self.proc else None
            if rc is not None:
                raise RuntimeError(
                    f"daemon exited during ingest (status {rc})")
            last_count = sum(len(fnames)
                             for _root, _dirs, fnames in os.walk(self.mnt))
            if last_count > expected_files:
                raise RuntimeError(
                    f"mounted tree contains {last_count} files; "
                    f"expected at most {expected_files}")
            if last_count == expected_files:
                rc = self.proc.poll() if self.proc else None
                if rc is not None:
                    raise RuntimeError(
                        f"daemon exited during ingest (status {rc})")
                return last_count
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"mounted tree contains {last_count} files after "
                    f"{timeout:.0f}s; expected {expected_files}")
            time.sleep(interval)
    def stop(self):
        if self.proc:
            subprocess.run(["fusermount3", "-u", self.mnt],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.proc.terminate()
            try: self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired: self.proc.kill()

def du_bytes(path) -> int:
    result = subprocess.run(
        ["du", "-sb", "--", str(path)], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return int(result.stdout.split()[0])

def stats_or_empty(d: DaemonProc, branch="main") -> dict:
    """stats.memory fields for `branch`; {} on before-builds (no command)."""
    try:
        r = d.ctl("stats.memory")
        if not r.get("ok"): return {}
        for b in r.get("branches", []):
            if b.get("name") == branch: return b
        return {}
    except Exception:
        return {}

def wait_for_fold(d: DaemonProc, label, timeout=60.0, interval=0.3):
    """Block until the background walk folds main's lazy-ingested delta into its
    authoritative shared base (delta_entries -> 0, base_entries > 0).

    Why this matters (mem-and-gc fix #4, also load-bearing for S2/G2):
    - WorkingTree::clone() shares the base O(1) ONLY if main's base is folded
      and authoritative; otherwise every clone COPIES the delta O(tree) ->
      RSS grows per branch -> G1 fails.
    - WorkingTree::remove() only erases a created-then-deleted path (no
      tombstone) when base_authoritative_ is true; if main is still
      non-authoritative, every remove leaves a tombstone -> delta grows
      unboundedly under churn -> G2 fails.
    The daemon auto-starts a background source walk at startup
    (main.cpp start_background -> bootstrap walk_bg -> WorkingTree::fold_into_base)
    which sets base_authoritative_ on completion, so callers must wait for it
    before checkpoint/clone/churn.

    On a BEFORE build stats.memory does not exist, so stats_or_empty returns {}
    and we skip the wait (the before build is the documented baseline that is
    EXPECTED to show per-branch RSS growth and tombstone accumulation because
    it has neither fold/clone-sharing nor tombstone hygiene).

    Bounded timeout: once stats.memory proves this is an after-build, failure
    to fold is a setup error. Raising prevents measurements from being taken
    against the known non-authoritative state.
    """
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        st = stats_or_empty(d)                      # {} on before-builds
        if not st:
            if label == "before":
                print(f"[fold:{label}] no stats.memory; skipping fold wait",
                      flush=True)
                return
            raise RuntimeError(
                f"fold:{label} could not read main stats.memory")
        last = st
        if st.get("base_entries", 0) > 0 and st.get("delta_entries", 1) == 0:
            print(f"[fold:{label}] base folded: base={st['base_entries']} "
                  f"delta={st['delta_entries']} "
                  f"shared_by={st.get('base_shared_by')}", flush=True)
            return
        time.sleep(interval)
    raise RuntimeError(
        f"fold:{label} timed out after {timeout:.0f}s: "
        f"base={last.get('base_entries') if last else '?'} "
        f"delta={last.get('delta_entries') if last else '?'}")

def write_csv(path, header, rows):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f); w.writerow(header); w.writerows(rows)

def workdir(tag: str) -> Path:
    root = Path(os.environ.get("MEMGC_WORK", f"/tmp/agentvfs-memgc-{os.getuid()}")) / tag
    if root.exists(): shutil.rmtree(root)
    root.mkdir(parents=True)
    return root
