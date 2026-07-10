import hashlib
import sys

import pytest

from src.agentvfs import StateChain
from src.llm_client import FakeActor
from src.runner_regular import RunConfig, run_regular
from src.verifier import ShadowModel, Verifier, VerificationError
from tests.conftest import STUB_ARGV, make_avfs

E1 = "# experiment 1\n"
E2 = "# experiment 2\n# REGRESS\n"


def test_chain_append_and_verify(daemon, tmp_path):
    a = make_avfs(daemon)
    v = Verifier(mount=daemon.mount, shadow=ShadowModel(), avfs=a,
                 results_dir=tmp_path)
    chain = StateChain(a)
    ck = a.checkpoint("c1")
    chain.append({"step": 0, "val_bpb": 2.5, "decision": "keep",
                  "train_py_sha": "x"}, fs_commit=ck, step=0)
    v.check_agent_chain(chain, "root-only")   # single record: describe path
    chain.append({"step": 1, "val_bpb": None, "decision": "failed",
                  "train_py_sha": "y"}, step=1)
    v.check_agent_chain(chain, "chain-ok")    # latest->parent walk
    assert v.failures == 0


def test_chain_verify_detects_divergence(daemon, tmp_path):
    a = make_avfs(daemon)
    v = Verifier(mount=daemon.mount, shadow=ShadowModel(), avfs=a,
                 results_dir=tmp_path)
    chain = StateChain(a)
    chain.append({"step": 0, "val_bpb": 1.0, "decision": "keep",
                  "train_py_sha": "x"}, step=0)
    chain.append({"step": 1, "val_bpb": 2.0, "decision": "keep",
                  "train_py_sha": "y"}, step=1)
    chain.records[1] = (chain.records[1][0], {"step": 1, "val_bpb": 9.9,
                                              "decision": "keep",
                                              "train_py_sha": "y"})
    with pytest.raises(VerificationError):
        v.check_agent_chain(chain, "chain-diverged")


def test_runner_records_chain_including_failed_step(daemon, tmp_path):
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel(); shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a,
                 results_dir=tmp_path / "r")
    chain = StateChain(a)
    actor = FakeActor(script=[(True, E1), (True, "# CRASH\n"), (True, E2)])
    cfg = RunConfig(steps=3, budget_s=1, timeout_s=60,
                    python_bin=sys.executable, trainer_argv=STUB_ARGV,
                    results_dir=tmp_path / "r")
    run_regular(daemon.mount, actor, cfg, avfs=a, verifier=v, shadow=shadow,
                chain=chain)
    payloads = [p for _, p in chain.records]
    assert [p["decision"] for p in payloads] == ["keep", "failed", "keep"]
    assert payloads[1]["val_bpb"] is None
    assert payloads[0]["train_py_sha"] == hashlib.sha256(E1.encode()).hexdigest()
    v.check_agent_chain(chain, "post-run")
    # the failed step's trainer output is persisted for diagnosis
    fail_log = tmp_path / "r" / "failed-step1.txt"
    assert fail_log.exists() and fail_log.read_text().strip()


def test_resume_check_restores_best_step(daemon, tmp_path):
    import main as sml_main
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel(); shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a,
                 results_dir=tmp_path / "r")
    chain = StateChain(a)
    actor = FakeActor(script=[(True, E1), (True, E2), (False, E1)])
    cfg = RunConfig(steps=3, budget_s=1, timeout_s=60,
                    python_bin=sys.executable, trainer_argv=STUB_ARGV,
                    results_dir=tmp_path / "r")
    steps, _ = run_regular(daemon.mount, actor, cfg, avfs=a, verifier=v,
                           shadow=shadow, chain=chain)
    out = sml_main.resume_check(a, chain, steps, daemon.mount, verifier=v)
    assert out["ok"] is True
    best = min((s for s in steps if s.val_bpb is not None),
               key=lambda s: s.val_bpb)
    assert out["step"] == best.step
    assert v.failures == 0   # end-of-run + post-rollback chain walks ran clean


def test_resume_check_detects_payload_divergence(daemon, tmp_path):
    import main as sml_main
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel(); shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a,
                 results_dir=tmp_path / "r")
    chain = StateChain(a)
    actor = FakeActor(script=[(True, E1), (True, E2)])
    cfg = RunConfig(steps=2, budget_s=1, timeout_s=60,
                    python_bin=sys.executable, trainer_argv=STUB_ARGV,
                    results_dir=tmp_path / "r")
    steps, _ = run_regular(daemon.mount, actor, cfg, avfs=a, verifier=v,
                           shadow=shadow, chain=chain)
    best = min((s for s in steps if s.val_bpb is not None),
               key=lambda s: s.val_bpb)
    i = next(i for i, (_, p) in enumerate(chain.records)
             if p["step"] == best.step)
    sid, p = chain.records[i]
    chain.records[i] = (sid, {**p, "val_bpb": 999.0})
    # restore returns the journal's payload, which no longer matches the
    # (tampered) harness record -> fail closed
    with pytest.raises(SystemExit):
        sml_main.resume_check(a, chain, steps, daemon.mount)
