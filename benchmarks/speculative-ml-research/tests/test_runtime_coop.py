import json
import subprocess
import sys
import threading
import time
from pathlib import Path

from tests.conftest import CTL_BIN, RUN_BIN, require_coop

TOY = Path(__file__).parent / "coop_toy.py"


def _ctl(sock, *args):
    r = subprocess.run([str(CTL_BIN), "--sock", sock, "--json", *args],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"ctl {args}: {r.stdout} {r.stderr}"
    return json.loads(r.stdout)


def _send(proc, line):
    proc.stdin.write(line + "\n")
    proc.stdin.flush()


def _read_ack(proc, prefix="ACK"):
    while True:
        line = proc.stdout.readline()
        assert line, "toy exited unexpectedly"
        if line.startswith(prefix):
            return line.strip()


class CoopToy:
    def __init__(self, daemon, rid="toy-1"):
        require_coop()
        self.rid = rid
        self.sock = daemon.sock
        self.proc = subprocess.Popen(
            [str(RUN_BIN), "--sock", daemon.sock, "--branch", "main",
             "--id", rid, "--", sys.executable, str(TOY)],
            cwd=daemon.mount, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1)
        # agentvfs-run prints the runtime id to the SHARED stdout first;
        # skip lines until the toy's READY.
        line = self.proc.stdout.readline()
        while line and not line.startswith("READY"):
            line = self.proc.stdout.readline()
        assert line.startswith("READY gen=1"), line

    def snapshot(self):
        # A boundary with no pending snapshot returns "continue" immediately
        # and is NOT re-armed, so: start the (blocking) snapshot request,
        # then keep OFFERING boundaries until it latches one. Extra offers
        # are harmless one-roundtrip no-ops.
        box = {}
        t = threading.Thread(target=lambda: box.update(_ctl(
            self.sock, "runtime", "snapshot", self.rid,
            "--timeout-ms", "10000")))
        t.start()
        deadline = time.time() + 15
        while t.is_alive() and time.time() < deadline:
            _send(self.proc, "boundary")
            _read_ack(self.proc)
            time.sleep(0.05)
        t.join(timeout=1)
        assert not t.is_alive() and box.get("ok"), box
        return box


def test_cooperative_snapshot_restore_roundtrip(daemon, tmp_path):
    toy = CoopToy(daemon)
    _send(toy.proc, "inc"); _read_ack(toy.proc)
    _send(toy.proc, "inc"); ack = _read_ack(toy.proc)
    assert "counter=2" in ack and "gen=1" in ack

    snap = toy.snapshot()                   # union captured at counter=2
    union_id = snap["union_state_id"]

    _send(toy.proc, "inc"); _read_ack(toy.proc)
    _send(toy.proc, "write after.txt"); _read_ack(toy.proc)
    assert (daemon.mount / "after.txt").read_text() == "counter=3\n"

    res = _ctl(toy.sock, "runtime", "restore", union_id)
    assert res.get("ok"), res
    # The restored grandchild re-emits the boundary ACK from the snapshot
    # point: snapshot-time counter, bumped generation.
    ack = _read_ack(toy.proc)
    assert "counter=2" in ack and "gen=2" in ack
    # Coupled fs rollback: the post-snapshot file is gone.
    assert not (daemon.mount / "after.txt").exists()
    # Memory proof: a fresh write shows the RESTORED counter.
    _send(toy.proc, "write restored.txt"); _read_ack(toy.proc)
    assert (daemon.mount / "restored.txt").read_text() == "counter=2\n"
    _send(toy.proc, "exit")
    toy.proc.wait(timeout=10)
    _ctl(toy.sock, "runtime", "drop", snap["template_id"])  # retire template
