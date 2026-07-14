#!/usr/bin/env python3
"""S1: RSS & resident entries vs branch count (spec curve 1)."""
import json, sys
from driver import (DaemonProc, gen_tree, stats_or_empty, write_csv, workdir,
                    wait_for_fold)


def sample_points(kmax):
    points = {k for k in (1, 2, 4, 8, 16, 24, 32, 48, 64) if k <= kmax}
    if kmax > 0:
        points.add(kmax)
    return points


def main():
    bin_path, label, out_csv = sys.argv[1], sys.argv[2], sys.argv[3]
    files = int(sys.argv[4]) if len(sys.argv) > 4 else 100_000
    dirs = int(sys.argv[5]) if len(sys.argv) > 5 else 10_000
    kmax = int(sys.argv[6]) if len(sys.argv) > 6 else 64

    wd = workdir(f"s1-{label}")
    generated = gen_tree(wd / "src", files, dirs)
    if generated != files:
        raise RuntimeError(f"generated {generated} source files; expected {files}")
    d = DaemonProc(bin_path, wd / "src", wd / "mnt", wd / "store", wd / "ctl.sock")
    rows = []
    try:
        # d.start() lives inside the try so a failed start (control socket
        # never came up -> RuntimeError) still triggers d.stop() in the finally:
        # DaemonProc.start() runs Popen (sets self.proc) BEFORE polling, and the
        # FUSE mount may already be live, so we must unmount + terminate to avoid
        # leaking a daemon/mount into the next scenario's workdir() rmtree.
        # stop() guards `if self.proc` and runs fusermount3 -u + terminate()+wait(),
        # so it is safe after a partially-successful start.
        d.start()
        n = d.force_full_ingest(generated)
        print(f"[s1:{label}] ingested {n} files", flush=True)
        # Wait for the background walk to fold main's delta into the shared
        # base BEFORE checkpoint/branch.create (fix #4). On before-builds this
        # is a no-op (stats.memory absent).
        wait_for_fold(d, label)
        r = d.ctl("checkpoint s1-base")
        assert r.get("ok"), r
        st = stats_or_empty(d)
        rows.append([label, 0, d.rss_kb(), st.get("base_entries", ""),
                     st.get("delta_entries", ""), st.get("base_shared_by", "")])
        print(f"[s1:{label}] k=0 rss={rows[-1][2]}kB base="
              f"{rows[-1][3]} delta={rows[-1][4]}", flush=True)
        samples = sample_points(kmax)
        for k in range(1, kmax + 1):
            # branch.create parses a JSON object (extract_str on "name"/"from"):
            # branch.create {"name":"fan001","from":"main"}  (fix #3)
            payload = json.dumps({"name": f"fan{k:03d}", "from": "main"})
            r = d.ctl(f"branch.create {payload}")
            assert r.get("ok"), r
            if k in samples:
                st = stats_or_empty(d)
                rows.append([label, k, d.rss_kb(), st.get("base_entries", ""),
                             st.get("delta_entries", ""), st.get("base_shared_by", "")])
                print(f"[s1:{label}] k={k} rss={rows[-1][2]}kB "
                      f"base_shared_by={rows[-1][5]}", flush=True)
    finally:
        d.stop()
        # write_csv inside the finally so a mid-scenario exception (assert /
        # ctl) still persists whatever rows were collected for diagnosis. With
        # no rows collected this writes a valid header-only CSV.
        write_csv(out_csv, ["bin_label", "k", "rss_kb", "base_entries",
                            "delta_entries", "base_shared_by"], rows)

if __name__ == "__main__":
    main()
