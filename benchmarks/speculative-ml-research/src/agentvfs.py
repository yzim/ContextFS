"""Thin subprocess wrapper over agentvfs-ctl with per-verb latency capture."""
import json
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path


class CtlError(RuntimeError):
    pass


@dataclass
class VerbRecord:
    verb: str
    step: int
    latency_us: int
    status: str
    store_bytes_after: int


# Verbs that add objects to the CAS. Only these re-sample the store size:
# `du` walks the whole object store, and branch create/delete run inside the
# speculation window, which must not pay an O(store) walk. peak_store_bytes
# stays exact — the store peaks right after a checkpoint or merge.
_STORE_GROWTH_VERBS = frozenset({"checkpoint", "merge"})


@dataclass
class AgentVFS:
    sock: str
    store_objects: Path
    ctl_bin: Path
    verbs: list = field(default_factory=list)
    _last_store_bytes: int = 0

    def _ctl(self, verb_name: str, args: list, step: int = -1, record: bool = True) -> str:
        cmd = [str(self.ctl_bin), "--sock", self.sock, *args]
        t0 = time.perf_counter_ns()
        r = subprocess.run(cmd, capture_output=True, text=True)
        us = (time.perf_counter_ns() - t0) // 1000
        status = "ok" if r.returncode == 0 else "error"
        if record:
            if verb_name in _STORE_GROWTH_VERBS:
                self._last_store_bytes = self.store_bytes()
            self.verbs.append(VerbRecord(verb_name, step, us, status,
                                         self._last_store_bytes))
        if r.returncode != 0:
            raise CtlError(f"agentvfs-ctl {' '.join(args)} failed rc={r.returncode}: "
                           f"{r.stdout.strip()} {r.stderr.strip()}")
        return r.stdout.strip()

    def checkpoint(self, label: str, step: int = -1,
                   branch: str | None = None) -> str:
        args = ["checkpoint", label] + (["--branch", branch] if branch else [])
        return self._ctl("checkpoint", args, step)

    def rollback(self, target: str, step: int = -1,
                 branch: str | None = None) -> None:
        args = ["rollback", target] + (["--branch", branch] if branch else [])
        self._ctl("rollback", args, step)

    def branch_create(self, name: str, step: int = -1) -> str:
        return self._ctl("branch_create", ["branch", "create", name, "--from", "main"], step)

    def branch_delete(self, name: str, step: int = -1) -> None:
        self._ctl("branch_delete", ["branch", "delete", name], step)

    def branch_merge(self, source: str, into: str = "main", step: int = -1) -> str:
        return self._ctl("merge", ["branch", "merge", source, "--into", into], step)

    def branch_list(self) -> list:
        out = self._ctl("branch_list", ["--json", "branch", "list"], record=False)
        data = json.loads(out)
        return data if isinstance(data, list) else data.get("branches", [])

    def status(self) -> dict:
        return json.loads(self._ctl("status", ["--json", "status"], record=False))

    def store_bytes(self) -> int:
        r = subprocess.run(["du", "-sb", str(self.store_objects)], capture_output=True, text=True)
        return int(r.stdout.split()[0]) if r.returncode == 0 and r.stdout else 0

    def session_register(self, cgroup_path: str, session_id: int, branch: str) -> None:
        self._ctl("session_register",
                  ["session", "register", "--cgroup", cgroup_path,
                   "--id", str(session_id), "--branch", branch], record=False)

    def session_unregister(self, cgroup_path: str) -> None:
        self._ctl("session_unregister",
                  ["session", "unregister", "--cgroup", cgroup_path], record=False)

    def _ctl_json(self, verb_name: str, args: list, step: int = -1,
                  record: bool = True) -> dict:
        out = self._ctl(verb_name, ["--json", *args], step=step, record=record)
        data = json.loads(out)
        if isinstance(data, dict) and data.get("ok") is False:
            raise CtlError(f"{verb_name}: {data.get('error')}")
        return data

    @staticmethod
    def _state_record(data: dict) -> dict:
        """Normalize a describe/latest response: unwrap the "state" envelope
        and alias payload_inline -> payload."""
        rec = dict(data.get("state") or data)
        if "payload" not in rec:
            rec["payload"] = rec.get("payload_inline", "")
        return rec

    # --- cooperative runtime (process state) ---------------------------------
    def runtime_snapshot(self, runtime_id: str, agent_state: str | None = None,
                         timeout_ms: int = 10000, step: int = -1) -> dict:
        args = ["runtime", "snapshot", runtime_id, "--timeout-ms", str(timeout_ms)]
        if agent_state:
            args += ["--agent-state", agent_state]
        return self._ctl_json("runtime_snapshot", args, step)

    def runtime_restore(self, union_state_id: str, timeout_ms: int = 10000,
                        step: int = -1) -> dict:
        return self._ctl_json("runtime_restore",
                              ["runtime", "restore", union_state_id,
                               "--timeout-ms", str(timeout_ms)], step)

    def runtime_status(self, runtime_id: str) -> dict:
        return self._ctl_json("runtime_status",
                              ["runtime", "status", runtime_id], record=False)

    def runtime_drop(self, template_id: str) -> None:
        self._ctl_json("runtime_drop", ["runtime", "drop", template_id],
                       record=False)

    # --- agent-state journal --------------------------------------------------
    # Durability contract: sync=True requires parent AND snapshot_base
    # referencing readable records; only synced appends update `state latest`.
    def state_append(self, agent_id: str, payload: str, kind: str = "session",
                     schema: str = "sml.step.v1", parent: str | None = None,
                     snapshot_base: str | None = None,
                     fs_commit: str | None = None,
                     union_state: str | None = None,
                     branch: str | None = None, sync: bool = False,
                     step: int = -1) -> str:
        args = ["state", "append", "--agent", agent_id, "--kind", kind,
                "--schema", schema, "--payload", payload]
        if parent:
            args += ["--parent", parent]
        if snapshot_base:
            args += ["--snapshot-base", snapshot_base]
        if fs_commit:
            args += ["--fs-commit", fs_commit]
        if union_state:
            args += ["--union-state", union_state]
        if branch:
            args += ["--branch", branch]
        if sync:
            args += ["--sync"]
        return self._ctl_json("state_append", args, step)["state_id"]

    def state_latest(self, agent_id: str, branch: str | None = None):
        args = ["state", "latest", "--agent", agent_id]
        if branch:
            args += ["--branch", branch]
        try:
            return self._state_record(
                self._ctl_json("state_latest", args, record=False))
        except CtlError as e:
            # only an unstarted chain maps to None; daemon loss and other
            # failures must not masquerade as an empty journal
            if "no latest ref" in str(e):
                return None
            raise

    def state_describe(self, state_id: str) -> dict:
        return self._state_record(
            self._ctl_json("state_describe", ["state", "describe", state_id],
                           record=False))

    def state_restore(self, state_id: str, mode: str = "session",
                      timeout_ms: int = 10000, step: int = -1) -> dict:
        return self._ctl_json("state_restore",
                              ["state", "restore", state_id, "--mode", mode,
                               "--timeout-ms", str(timeout_ms)], step)


class StateChain:
    """Harness-side ground truth for the agent-state journal.

    Durability scheme (daemon contract): the FIRST record is a logical-only
    root (no --sync; describable but not "latest"); every later record is
    synced with --parent <previous> --snapshot-base <root>, which publishes
    the latest ref."""

    def __init__(self, avfs: "AgentVFS", agent_id: str = "sml-actor"):
        self.avfs = avfs
        self.agent_id = agent_id
        self.records: list = []   # [(state_id, payload_dict)]
        self.root_id: str | None = None
        self.last_id: str | None = None
        self.trees: dict = {}     # {state_id: hash_tree at that step}

    def append(self, payload: dict, fs_commit: str | None = None,
               step: int = -1, tree=None) -> str:
        body = json.dumps(payload, sort_keys=True)
        if self.root_id is None:
            sid = self.avfs.state_append(self.agent_id, body,
                                         fs_commit=fs_commit, step=step)
            self.root_id = sid
        else:
            sid = self.avfs.state_append(self.agent_id, body,
                                         parent=self.last_id,
                                         snapshot_base=self.root_id,
                                         fs_commit=fs_commit, sync=True,
                                         step=step)
        self.records.append((sid, payload))
        self.last_id = sid
        if tree is not None:
            self.trees[sid] = tree
        return sid


# --- daemon lifecycle, shared by main.py and the test fixture ---------------
# The argv contract and the readiness signal (control socket + a sentinel file
# visible through the mount) live here so tests exercise the same protocol
# the real entrypoint uses.

def daemon_argv(bin_path, source, mountpoint, store, sock) -> list:
    return [str(bin_path), "--source", str(source), "--mountpoint", str(mountpoint),
            "--store", str(store), "--control-sock", str(sock), "-f"]


def wait_daemon_ready(sock, sentinel: Path, tries: int = 200,
                      interval: float = 0.05) -> bool:
    for _ in range(tries):
        if Path(sock).is_socket() and Path(sentinel).exists():
            return True
        time.sleep(interval)
    return False


def stop_daemon(proc, mount) -> None:
    subprocess.run(["fusermount3", "-u", str(mount)], capture_output=True)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
