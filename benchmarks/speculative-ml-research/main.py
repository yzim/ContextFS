"""Entrypoint: wires config + daemon + preconditions and dispatches a runner."""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import yaml

from src.agentvfs import (AgentVFS, StateChain, daemon_argv, stop_daemon,
                          wait_daemon_ready)
from src.cgroup import CgroupSession, CgroupUnavailable
from src.llm_client import Actor, Speculator
from src.runner_regular import RunConfig, run_regular
from src.runner_spec import replay_check, run_spec
from src.verifier import ShadowModel, Verifier
from src.warm import WarmTrainer
from src import environment, overlay

HERE = Path(__file__).resolve().parent


def die(msg: str) -> None:
    print(f"FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def load_cfg(path: str, mode: str):
    cfg = yaml.safe_load(Path(path).read_text())
    roles = ("actor", "speculator") if mode == "spec" else ("actor",)
    for role in roles:
        for key in ("base_url_env", "api_key_env"):
            env = cfg[role][key]
            if not os.environ.get(env):
                die(f"env var {env} (for {role}.{key}) is not set")
    return cfg


def make_llm(cfg, role, cls):
    c = cfg[role]
    return cls(model=c["model"], base_url=os.environ[c["base_url_env"]],
               api_key=os.environ[c["api_key_env"]], timeout_s=c["timeout_s"],
               max_tokens=c.get("max_tokens"))


def start_daemon(cfg, run_dir: Path):
    src = run_dir / "seed"
    mount = run_dir / "mnt"
    store = run_dir / "store"
    sock = str(run_dir / "control.sock")
    environment.seed_base_tree(src, HERE / cfg["paths"]["autoresearch"])
    overlay.apply_overlay(src)
    mount.mkdir(parents=True, exist_ok=True)
    with open(run_dir / "daemon.log", "wb") as log:
        proc = subprocess.Popen(
            daemon_argv(HERE / cfg["paths"]["agentvfs_bin"], src, mount, store, sock),
            stdout=log, stderr=subprocess.STDOUT)
    if not wait_daemon_ready(sock, mount / "train.py"):
        die("agentvfs daemon failed to start; see daemon.log")
    return proc, mount, sock, store, src


def preconditions(avfs, mount):
    avfs.status()  # daemon reachable
    try:
        probe = CgroupSession("sml-probe")
    except CgroupUnavailable as e:
        die(f"cgroup v2 unavailable: {e} (run as root or set SML_CGROUP_ROOT)")
    avfs.checkpoint("precondition-base")
    avfs.branch_create("sml-probe-branch")
    avfs.session_register(probe.path, session_id=999, branch="sml-probe-branch")
    probe.run_text(["bash", "-c", "echo probe > .sml-probe.txt"], cwd=mount)
    if (mount / ".sml-probe.txt").exists():
        die("branch routing NOT isolating: probe write visible on main")
    avfs.session_unregister(probe.path)
    probe.destroy()
    avfs.branch_delete("sml-probe-branch")
    print("preconditions ok: daemon, cgroup routing, isolation")


def data_preconditions(rc):
    """Real training resolves its dataset via ~/.cache/autoresearch
    (prepare.py expanduser) — a wrong HOME (e.g. `sudo env` resets it to
    /root) fails every experiment in ~0.1s AFTER the API spend. Abort
    before any daemon or LLM work; stub runs (trainer_argv set) need no
    dataset."""
    if rc.trainer_argv is not None:
        return
    tok = Path("~/.cache/autoresearch/tokenizer/tokenizer.pkl").expanduser()
    if not tok.exists():
        die(f"autoresearch dataset not found: {tok} (HOME="
            f"{os.environ.get('HOME')}). Under sudo, preserve the invoking "
            f"user's HOME (sudo env HOME=\"$HOME\" ...); or run prepare.py "
            f"once for this user.")


def resume_check(avfs, chain, steps, mount, verifier=None):
    """Agent-state ROLLBACK check: state restore --mode full on the best
    step's record must roll main back to that step's checkpoint tree and
    return that step's payload. With a verifier, the journal chain is
    shadow-verified at end of run and again after the rollback (a full
    restore must not disturb the journal)."""
    from src.verifier import hash_tree
    if verifier is not None:
        verifier.check_agent_chain(chain, "end-of-run")
    ok_steps = [s for s in steps if s.val_bpb is not None]
    if not ok_steps or not chain.records:
        return {"ok": True, "step": None, "state_id": None}
    best = min(ok_steps, key=lambda s: s.val_bpb)
    hits = [(sid, p) for sid, p in chain.records if p["step"] == best.step]
    if not hits:
        die(f"resume check FAILED: no chain record for best step {best.step}")
    sid, expected_payload = hits[0]
    rec = avfs.state_describe(sid)
    res = avfs.state_restore(sid, mode="full")
    returned = AgentVFS._state_record(res)
    payload_ok = json.loads(returned.get("payload") or "null") == expected_payload
    tree_ok = hash_tree(mount) == chain.trees.get(sid)
    ok = (res.get("rolled_back_to") == rec.get("fs_commit")) and tree_ok \
        and payload_ok
    if not ok:
        die(f"resume check FAILED: state {sid} step {best.step} "
            f"(rolled_back_to={res.get('rolled_back_to')}, tree_ok={tree_ok}, "
            f"payload_ok={payload_ok})")
    if verifier is not None:
        verifier.check_agent_chain(chain, "post-resume-rollback")
    return {"ok": True, "step": best.step, "state_id": sid}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", required=True,
                    choices=["regular", "regular-native", "spec"])
    ap.add_argument("--trainer", choices=["fresh", "warm"], default="fresh")
    ap.add_argument("--config", default="config.yml")
    ap.add_argument("--run-dir", default=None)
    args = ap.parse_args()

    cfg = load_cfg(args.config, args.mode)
    run_id = f"{args.mode}-{time.strftime('%Y%m%d-%H%M%S')}"
    run_dir = Path(args.run_dir or f"/tmp/sml-{run_id}")
    run_dir.mkdir(parents=True, exist_ok=True)
    results_dir = HERE / "results" / run_id
    # fail fast: results are only written at the END of a (potentially
    # hour-long) run — a stale root-owned results/ from a previous sudo run
    # must abort now, not after the compute and API spend
    try:
        results_dir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        die(f"cannot create {results_dir}: {e} "
            f"(root-owned leftover from a sudo run? chown or remove results/)")

    rc = RunConfig(steps=cfg["steps"], budget_s=cfg["train_budget_s"],
                   timeout_s=cfg["train_timeout_s"], python_bin=sys.executable,
                   trainer_argv=None, results_dir=results_dir)
    data_preconditions(rc)
    actor = make_llm(cfg, "actor", Actor)

    if args.mode == "regular-native":
        if args.trainer == "warm":
            die("warm trainer is Linux/cooperative-only; incompatible with "
                "--mode regular-native")
        work = run_dir / "native"
        environment.seed_base_tree(work, HERE / cfg["paths"]["autoresearch"])
        overlay.apply_overlay(work)
        steps, wall = run_regular(work, actor, rc, avfs=None)
        print(f"native wall_s={wall:.1f}")
        return

    proc, mount, sock, store, src = start_daemon(cfg, run_dir)
    warm = None
    try:
        avfs = AgentVFS(sock=sock, store_objects=store / "objects",
                        ctl_bin=HERE / cfg["paths"]["ctl_bin"])
        preconditions(avfs, mount)
        shadow = ShadowModel()
        shadow.snapshot_baseline(mount)
        verifier = Verifier(mount=mount, shadow=shadow, avfs=avfs,
                            results_dir=results_dir)
        chain = StateChain(avfs)
        if args.trainer == "warm":
            probe = ("import torch" if rc.trainer_argv is None else "pass")
            t0 = time.time()
            subprocess.run([sys.executable, "-c", probe], check=True)
            rc.startup_baseline_s = time.time() - t0
            chain.append({"run": run_id, "step": -1}, step=-1)
            warm = WarmTrainer(avfs, mount, sys.executable, run_dir,
                               budget_s=cfg["train_budget_s"],
                               warm_import=("torch" if rc.trainer_argv is None
                                            else "none"))
            warm.launch(agent_state=chain.root_id)
        if args.mode == "regular":
            steps, wall = run_regular(mount, actor, rc, avfs=avfs,
                                      verifier=verifier, shadow=shadow,
                                      chain=chain, warm=warm)
            print(f"regular(agentvfs) wall_s={wall:.1f} verify_failures={verifier.failures}")
        else:
            speculator = make_llm(cfg, "speculator", Speculator)
            steps, wall, committed = run_spec(
                mount, actor, speculator, cfg["k"], rc, avfs, verifier, shadow,
                cg_factory=CgroupSession, chain=chain, warm=warm,
                k_launch=cfg.get("k_launch"))
            (results_dir / "committed.json").write_text(json.dumps(committed))
            base = (src / "train.py").read_text()
            mount_final = (mount / "train.py").read_text()
            ok, traj = replay_check(committed, base, rc, run_dir / "replay",
                                    expected_final=mount_final, seed_dir=src)
            # failed steps commit nothing and leave no trace on main — the
            # replay covers successful (committed) steps only
            spec_traj = [s.val_bpb for s in steps if s.val_bpb is not None]
            traj_match = traj == spec_traj  # exact on CPU with fixed seeds
            (results_dir / "lossless.json").write_text(json.dumps(
                {"content_match": ok, "trajectory_match": traj_match,
                 "replay_trajectory": traj, "speculative_trajectory": spec_traj}))
            if ok and not traj_match:
                print("NOTE: content lossless but metric drift (environment noise "
                      "per spec) — see lossless.json")
            print(f"spec wall_s={wall:.1f} lossless_content={ok} "
                  f"lossless_trajectory={traj_match} "
                  f"verify_failures={verifier.failures}")
        rc_out = resume_check(avfs, chain, steps, mount, verifier=verifier)
        print(f"resume_check ok={rc_out['ok']} step={rc_out['step']}")
        (results_dir / "resume.json").write_text(json.dumps(rc_out))
    finally:
        if warm:
            warm.close()
        stop_daemon(proc, mount)


if __name__ == "__main__":
    main()
