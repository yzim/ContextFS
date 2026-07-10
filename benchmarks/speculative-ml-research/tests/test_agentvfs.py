import re

from src.agentvfs import CtlError
from tests.conftest import make_avfs as make

HASH_RE = re.compile(r"^[0-9a-f]{64}$")


def test_checkpoint_returns_hash_and_records_verb(daemon):
    a = make(daemon)
    h = a.checkpoint("c1", step=0)
    assert HASH_RE.match(h)
    assert a.verbs[-1].verb == "checkpoint"
    assert a.verbs[-1].status == "ok"
    assert a.verbs[-1].latency_us > 0


def test_rollback_restores_content(daemon):
    a = make(daemon)
    p = daemon.mount / "f.txt"
    p.write_text("v1")
    a.checkpoint("c1")
    p.write_text("v2")
    a.rollback("c1")
    assert p.read_text() == "v1"


def test_branch_create_list_merge_delete(daemon):
    a = make(daemon)
    a.checkpoint("base")
    a.branch_create("spec-0")
    names = [b.get("name") for b in a.branch_list()]
    assert "spec-0" in names
    merge_commit = a.branch_merge("spec-0", into="main")
    assert HASH_RE.match(merge_commit)
    a.branch_delete("spec-0")
    assert "spec-0" not in [b.get("name") for b in a.branch_list()]


def test_ctl_error_raises(daemon):
    a = make(daemon)
    try:
        a.rollback("no-such-label-xyz")
        assert False, "expected CtlError"
    except CtlError:
        pass
    assert a.verbs[-1].status == "error"


def test_store_bytes_positive(daemon):
    a = make(daemon)
    a.checkpoint("c1")
    assert a.store_bytes() > 0


def test_create_immediately_after_rollback(daemon):
    """Kernel dentry-cache coherence: rollback removes a path from the working
    tree; re-creating it IMMEDIATELY must succeed. With nonzero FUSE entry
    timeouts the kernel skips LOOKUP on the still-cached dentry, sends OPEN
    instead of CREATE, and the daemon returns ENOENT for a file the caller
    is trying to create (regression: real-run abort on .train-log.txt)."""
    a = make(daemon)
    a.checkpoint("pre")
    p = daemon.mount / "newfile.txt"
    p.write_bytes(b"hello")
    p.stat()  # populate kernel dentry + attr cache
    a.rollback("pre")
    with open(p, "wb") as f:  # no sleep: must work inside the 1s cache window
        f.write(b"again")
    assert p.read_bytes() == b"again"
