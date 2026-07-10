import json

import pytest

from src.agentvfs import CtlError
from tests.conftest import make_avfs, require_coop
from tests.test_runtime_coop import CoopToy, _read_ack, _send


def test_state_chain_root_then_synced_descendant(daemon):
    a = make_avfs(daemon)
    p1 = json.dumps({"step": 0, "val_bpb": 2.5, "decision": "keep"})
    root = a.state_append("sml-actor", p1)          # logical-only root
    assert len(root) == 64
    d1 = a.state_describe(root)                      # describable pre-latest
    assert json.loads(d1["payload"]) == json.loads(p1)
    p2 = json.dumps({"step": 1, "val_bpb": None, "decision": "failed"})
    s2 = a.state_append("sml-actor", p2, parent=root, snapshot_base=root,
                        sync=True)
    latest = a.state_latest("sml-actor")             # published by the sync
    assert latest["state_id"] == s2
    assert json.loads(latest["payload"]) == json.loads(p2)
    assert latest["parent_state_id"] == root


def test_state_append_links_fs_commit(daemon):
    a = make_avfs(daemon)
    (daemon.mount / "f.txt").write_text("v1")
    ck = a.checkpoint("c1")
    sid = a.state_append("sml-actor", "{}", fs_commit=ck)
    assert a.state_describe(sid)["fs_commit"] == ck


def test_state_restore_full_rolls_back_fs(daemon):
    a = make_avfs(daemon)
    (daemon.mount / "f.txt").write_text("v1")
    ck = a.checkpoint("c1")
    sid = a.state_append("sml-actor", "{}", fs_commit=ck)
    (daemon.mount / "f.txt").write_text("v2")
    a.checkpoint("c2")
    res = a.state_restore(sid, mode="full")
    assert res["rolled_back_to"] == ck
    assert (daemon.mount / "f.txt").read_text() == "v1"


def test_state_restore_session_returns_chain(daemon):
    a = make_avfs(daemon)
    s1 = a.state_append("sml-actor", '{"n":1}')
    s2 = a.state_append("sml-actor", '{"n":2}', parent=s1, snapshot_base=s1,
                        sync=True)
    res = a.state_restore(s2, mode="session")
    ids = [r["state_id"] for r in res["chain"]]
    assert ids[0] == s2 and s1 in ids


def test_branch_scoped_checkpoint_rollback(daemon):
    a = make_avfs(daemon)
    a.checkpoint("base")
    a.branch_create("side")
    ck = a.checkpoint("side-1", branch="side")
    a.rollback(ck, branch="side")
    a.branch_delete("side")


def test_state_restore_mode_runtime_restores_process(daemon):
    require_coop()
    a = make_avfs(daemon)
    toy = CoopToy(daemon, rid="toy-rt")
    _send(toy.proc, "inc"); _read_ack(toy.proc)
    snap = toy.snapshot()
    sid = a.state_append("sml-actor", '{"n":1}',
                         union_state=snap["union_state_id"])
    _send(toy.proc, "inc"); _read_ack(toy.proc)
    res = a.state_restore(sid, mode="runtime")
    assert res["runtime"]["template_id"] == snap["template_id"]
    ack = _read_ack(toy.proc)      # grandchild re-emits the boundary ACK
    assert "counter=1" in ack and "gen=2" in ack
    _send(toy.proc, "exit"); toy.proc.wait(timeout=10)
    a.runtime_drop(snap["template_id"])


def test_runtime_drop_then_restore_fails(daemon):
    a = make_avfs(daemon)
    toy = CoopToy(daemon, rid="toy-drop")
    snap = toy.snapshot()
    a.runtime_drop(snap["template_id"])
    # the parked template consumes the drop within ~5ms and its record is
    # reaped — a later restore errors ("unknown template" typically)
    import time as _t
    _t.sleep(0.2)
    with pytest.raises(CtlError):
        a.runtime_restore(snap["union_state_id"])
    _send(toy.proc, "exit"); toy.proc.wait(timeout=10)


def test_runtime_restore_unknown_union_errors(daemon):
    a = make_avfs(daemon)
    with pytest.raises(CtlError):
        a.runtime_restore("0" * 64)


def test_state_latest_none_only_for_missing_ref(daemon):
    """'no latest ref' means an unstarted chain; every other failure
    (e.g. daemon loss) must surface, not masquerade as an empty journal."""
    a = make_avfs(daemon)
    assert a.state_latest("never-appended") is None
    a.sock = "/tmp/definitely-not-a-daemon.sock"
    with pytest.raises(CtlError):
        a.state_latest("never-appended")
