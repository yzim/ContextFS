import hashlib

import pytest

from src.verifier import ShadowModel, Verifier, VerificationError, hash_tree
from tests.conftest import make_avfs


def sha(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()


def test_hash_tree_and_shadow_expected(tmp_path):
    (tmp_path / "a.txt").write_text("A")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "b.txt").write_text("B")
    h = hash_tree(tmp_path)
    assert h == {"a.txt": sha("A"), "sub/b.txt": sha("B")}

    sm = ShadowModel()
    sm.snapshot_baseline(tmp_path)
    sm.record_edit("main", "a.txt", "A2")
    assert sm.expected("main")["a.txt"] == sha("A2")
    assert sm.expected("main")["sub/b.txt"] == sha("B")

    sm.fork("main", "spec-0")
    sm.record_edit("spec-0", "a.txt", "A3")
    assert sm.expected("spec-0")["a.txt"] == sha("A3")
    assert sm.expected("main")["a.txt"] == sha("A2")


def test_verifier_check_main_pass_and_fail(daemon, tmp_path):
    a = make_avfs(daemon)
    sm = ShadowModel()
    sm.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=sm, avfs=a, results_dir=tmp_path)

    v.check_main("baseline-ok")  # untouched mount matches baseline

    (daemon.mount / "base.txt").write_text("tampered")  # mutate WITHOUT recording
    with pytest.raises(VerificationError):
        v.check_main("tamper-detected")
    assert v.failures == 1
    assert (tmp_path / "verify-fail-tamper-detected.txt").exists()

    sm.record_edit("main", "base.txt", "tampered")  # reconcile
    v.check_main("reconciled")


def test_verifier_rollback_roundtrip(daemon, tmp_path):
    a = make_avfs(daemon)
    sm = ShadowModel()
    sm.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=sm, avfs=a, results_dir=tmp_path)

    state_before = hash_tree(daemon.mount)
    a.checkpoint("ck1")
    (daemon.mount / "base.txt").write_text("changed")
    a.rollback("ck1")
    v.check_rollback(state_before, "rollback-roundtrip")
