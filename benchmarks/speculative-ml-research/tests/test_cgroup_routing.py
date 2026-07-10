import pytest

from tests.conftest import cgroup_or_skip, make_avfs


@pytest.fixture
def cg():
    s = cgroup_or_skip("smltest")
    yield s
    s.destroy()


def test_branch_routing_isolation_and_merge(daemon, cg):
    a = make_avfs(daemon)
    a.checkpoint("base")
    a.branch_create("feature")
    a.session_register(cg.path, session_id=1, branch="feature")

    # write inside the cgroup -> lands on 'feature'
    cg.run_text(["bash", "-c", "echo source > source.txt"], cwd=daemon.mount)

    # main writes its own file
    (daemon.mount / "target.txt").write_text("target\n")

    # isolation both ways
    assert not (daemon.mount / "source.txt").exists(), "main must not see branch write"
    seen = cg.run_text(["cat", "source.txt"], cwd=daemon.mount)
    assert seen.strip() == "source", "branch must see its own write"
    rc_out = cg.run_text(["bash", "-c", "test -e target.txt && echo yes || echo no"],
                         cwd=daemon.mount)
    assert rc_out.strip() == "no", "branch must not see main write"

    # merge lands the branch write on main
    a.branch_merge("feature", into="main")
    assert (daemon.mount / "source.txt").read_text().strip() == "source"
    assert (daemon.mount / "target.txt").read_text().strip() == "target"

    a.session_unregister(cg.path)
    a.branch_delete("feature")


def test_kill_all_terminates_processes(daemon, cg):
    a = make_avfs(daemon)
    a.checkpoint("base")
    a.branch_create("spec-kill")
    a.session_register(cg.path, session_id=2, branch="spec-kill")
    p = cg.run(["sleep", "300"], cwd=daemon.mount)
    cg.kill_all()
    assert p.wait(timeout=10) != 0
    a.session_unregister(cg.path)
    a.branch_delete("spec-kill")
