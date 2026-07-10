"""Shadow model of expected file state per branch + postcondition checks."""
import hashlib
from dataclasses import dataclass, field
from pathlib import Path


class VerificationError(RuntimeError):
    pass


# Paths ignored by BOTH hashing sides (host hash_tree and check_branch's
# in-cgroup find) — grow this in one place or the two views silently disagree.
EXCLUDE = (".agentvfs",)


def hash_tree(root: Path, exclude: tuple = EXCLUDE) -> dict:
    out = {}
    root = Path(root)
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if any(rel == e or rel.startswith(e + "/") for e in exclude):
            continue
        if p.is_file() and not p.is_symlink():
            out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


class ShadowModel:
    def __init__(self):
        self._branches: dict[str, dict] = {}

    def snapshot_baseline(self, tree: Path) -> None:
        self._branches["main"] = dict(hash_tree(tree))

    def record_edit(self, branch: str, relpath: str, content: str) -> None:
        self._branches[branch][relpath] = hashlib.sha256(content.encode()).hexdigest()

    def set_state(self, branch: str, state: dict) -> None:
        self._branches[branch] = dict(state)

    def fork(self, parent: str, child: str) -> None:
        self._branches[child] = dict(self._branches[parent])

    def merge(self, source: str, into: str) -> None:
        # mirror of agentvfs `branch merge`: the target adopts the source's
        # tracked state wholesale (replace, not union) — if agentvfs merge
        # semantics ever change, this is the expectation to update
        self._branches[into] = dict(self._branches[source])

    def drop(self, branch: str) -> None:
        self._branches.pop(branch, None)

    def resync_tracked(self, branch: str, full_state: dict) -> None:
        """Adopt full_state, restricted to the paths this shadow already tracks.

        Used after a verified rollback: the caller's full_state is a complete
        hash_tree snapshot, which includes volatile training artifacts the
        harness cannot predict (trainer logs, __pycache__/ in real runs).
        The rollback check must see the full snapshot (stronger agentvfs
        restoration test), but the shadow tracks harness EDITS only —
        injecting any volatile artifact here would make the next check abort
        on a stale hash. Intersecting with the already-tracked keys drops
        ALL volatile artifacts without hardcoding filenames.
        """
        tracked = self._branches[branch]
        self._branches[branch] = {k: v for k, v in full_state.items() if k in tracked}

    def expected(self, branch: str) -> dict:
        return dict(self._branches[branch])


@dataclass
class Verifier:
    mount: Path
    shadow: ShadowModel
    avfs: object
    results_dir: Path
    failures: int = field(default=0)

    def _diff(self, expected: dict, actual: dict) -> list[str]:
        lines = []
        for k in sorted(set(expected) | set(actual)):
            e, a = expected.get(k), actual.get(k)
            if e != a:
                lines.append(f"{k}: expected={e} actual={a}")
        return lines

    def _fail(self, label: str, diff: list[str]) -> None:
        self.failures += 1
        self.results_dir.mkdir(parents=True, exist_ok=True)
        dump = self.results_dir / f"verify-fail-{label}.txt"
        parts = [f"POSTCONDITION FAILED: {label}", "", *diff, ""]
        try:
            parts += ["-- agentvfs status --", str(self.avfs.status()),
                      "-- branch list --", str(self.avfs.branch_list())]
        except Exception as e:  # daemon may be gone; still dump the diff
            parts += [f"(ctl unavailable: {e})"]
        dump.write_text("\n".join(parts))
        raise VerificationError(f"{label}: {len(diff)} mismatches, see {dump}")

    def _check(self, expected: dict, actual: dict, label: str) -> None:
        # expected covers tracked paths; actual may contain extra untracked
        # artifacts (training outputs) which are not violations.
        diff = self._diff(expected, {k: v for k, v in actual.items() if k in expected})
        if diff:
            self._fail(label, diff)

    def check_main(self, label: str) -> dict:
        actual = hash_tree(self.mount)
        self._check(self.shadow.expected("main"), actual, label)
        return actual

    def check_branch(self, branch: str, cg, label: str) -> None:
        prunes = " -o ".join(f"-path ./{e} -prune" for e in EXCLUDE)
        out = cg.run_text(
            ["bash", "-c",
             rf'find . \( {prunes} \) -o -type f -print0 | sort -z | '
             r'xargs -0 -r sha256sum'],
            cwd=self.mount, timeout=120)
        actual = {}
        for line in out.splitlines():
            digest, _, name = line.partition("  ")
            actual[name.removeprefix("./")] = digest
        self._check(self.shadow.expected(branch), actual, label)

    def check_rollback(self, expected_state: dict, label: str) -> dict:
        actual = hash_tree(self.mount)
        self._check(expected_state, actual, label)
        return actual

    def check_agent_chain(self, chain, label: str) -> None:
        """Walk the journal and compare (state_id, payload) sequences against
        the harness ground truth. A single-record chain has no latest ref
        (the root is logical-only), so it is verified via describe."""
        import json as _json

        def _norm(rec):
            return (rec["state_id"], _json.loads(rec["payload"]))

        if not chain.records:
            return
        if len(chain.records) == 1:
            actual = [_norm(self.avfs.state_describe(chain.root_id))]
        else:
            actual = []
            rec = self.avfs.state_latest(chain.agent_id)
            hops = 0
            while rec is not None and hops <= len(chain.records) + 1:
                actual.append(_norm(rec))
                parent = rec.get("parent_state_id") or ""
                rec = (self.avfs.state_describe(parent)
                       if parent and set(parent) != {"0"} else None)
                hops += 1
            actual.reverse()
        expected = [(sid, p) for sid, p in chain.records]
        if actual != expected:
            diff = [f"expected {len(expected)} records, got {len(actual)}"]
            for i, pair in enumerate(zip(expected, actual)):
                if pair[0] != pair[1]:
                    diff.append(f"[{i}] expected={pair[0]} actual={pair[1]}")
            self._fail(label, diff)
