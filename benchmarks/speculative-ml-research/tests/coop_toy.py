"""Cooperative toy runtime driven over stdin (launched via agentvfs-run).

The counter lives ONLY in process memory; the only way it reaches disk is an
explicit `write` command — a post-restore `write` therefore proves the restore
resumed process MEMORY (not files). All generations inherit the same
stdin/stdout pipe; the daemon SIGSTOPs the old generation at restore-begin and
retires it after the new one is ready, so exactly one generation reads stdin
at a time. After a restore the grandchild re-emits the boundary ACK from the
snapshot point — that duplicate ACK (snapshot-time counter, new generation)
is the signal tests assert on.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from runtime_boundary import boundary, generation  # noqa: E402

counter = 0
print(f"READY gen={generation()}", flush=True)
while True:
    line = sys.stdin.readline()
    if not line:
        break
    cmd = line.strip().split()
    if not cmd:
        continue
    if cmd[0] == "exit":
        break
    if cmd[0] == "inc":
        counter += 1
    elif cmd[0] == "write":
        Path(cmd[1]).write_text(f"counter={counter}\n")
    elif cmd[0] == "boundary":
        boundary("toy")
    print(f"ACK {cmd[0]} counter={counter} gen={generation()}", flush=True)
