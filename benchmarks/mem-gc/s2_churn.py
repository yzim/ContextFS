#!/usr/bin/env python3
"""S2: resident entries & checkpoint latency vs churn ops (spec curve 2).

Each cycle creates CREATE_N files under <mnt>/churn/, overwrites half, renames
a quarter (renamed then renamed back), then deletes ALL of them — pure
created-then-deleted churn. A checkpoint is taken every CP_EVERY cycles and the
live memory stats + checkpoint latency are sampled.

On the AFTER build, tombstone hygiene (WorkingTree::remove_locked) erases each
created-then-deleted path from the delta once main's base is authoritative, so
the delta returns to ~empty after every cycle and resident entries stay flat
(G2 PASS). On the BEFORE build there is no hygiene: every remove leaves a
tombstone and the delta grows without bound (documented baseline).
"""
import os, sys, time
from pathlib import Path
from driver import (DaemonProc, gen_tree, stats_or_empty, write_csv, workdir,
                    wait_for_fold)

CREATE_N, CP_EVERY = 200, 5


def main():
    bin_path, label, out_csv = sys.argv[1], sys.argv[2], sys.argv[3]
    files = int(sys.argv[4]) if len(sys.argv) > 4 else 20_000
    dirs = int(sys.argv[5]) if len(sys.argv) > 5 else 2_000
    cycles = int(sys.argv[6]) if len(sys.argv) > 6 else 100

    wd = workdir(f"s2-{label}")
    generated = gen_tree(wd / "src", files, dirs)
    if generated != files:
        raise RuntimeError(f"generated {generated} source files; expected {files}")
    d = DaemonProc(bin_path, wd / "src", wd / "mnt", wd / "store", wd / "ctl.sock")
    rows, ops = [], 0
    try:
        d.start()
        n = d.force_full_ingest(generated)
        print(f"[s2:{label}] ingested {n} files", flush=True)
        # Wait for the background walk to fold main's lazy-ingested delta into
        # its authoritative base BEFORE any churn. This is load-bearing for G2:
        # WorkingTree::remove() only erases a created-then-deleted path (no
        # tombstone) once base_authoritative_ is true; without the fold every
        # remove would leave a tombstone and the delta would grow unboundedly.
        # Placed BEFORE churn.mkdir() so a pristine delta is observed. No-op on
        # the before build (stats.memory absent).
        wait_for_fold(d, label)
        churn = Path(d.mnt) / "churn"
        churn.mkdir(exist_ok=True)
        r = d.ctl("checkpoint s2-base"); assert r.get("ok"), r
        for cyc in range(cycles):
            names = [churn / f"c{cyc:04d}-{i:03d}.txt" for i in range(CREATE_N)]
            for p in names:
                p.write_bytes(b"payload"); ops += 1
            for p in names[::2]:
                p.write_bytes(b"payload2"); ops += 1          # overwrite half
            for p in names[::4]:                              # rename a quarter
                q = p.with_suffix(".ren")
                os.rename(p, q); os.rename(q, p); ops += 2
            for p in names:
                p.unlink(); ops += 1                          # delete all
            cp_ms = ""
            if (cyc + 1) % CP_EVERY == 0:
                t0 = time.monotonic()
                r = d.ctl(f"checkpoint s2-cyc{cyc:04d}"); assert r.get("ok"), r
                cp_ms = round((time.monotonic() - t0) * 1000, 1)
                st = stats_or_empty(d)
                rows.append([label, ops, d.rss_kb(), st.get("base_entries", ""),
                             st.get("delta_entries", ""),
                             st.get("delta_tombstones", ""), cp_ms])
                print(f"[s2:{label}] ops={ops} rss={rows[-1][2]}kB "
                      f"base={rows[-1][3]} delta={rows[-1][4]} "
                      f"tomb={rows[-1][5]} cp={cp_ms}ms", flush=True)
    finally:
        d.stop()
        # write_csv in finally so a mid-scenario exception (assert / ctl) still
        # persists whatever rows were collected for diagnosis; with no rows this
        # writes a valid header-only CSV that report.py's load() handles.
        write_csv(out_csv, ["bin_label", "ops", "rss_kb", "base_entries",
                            "delta_entries", "delta_tombstones",
                            "checkpoint_ms"], rows)


if __name__ == "__main__":
    main()
