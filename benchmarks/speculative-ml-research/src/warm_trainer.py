"""Cooperative warm trainer driver, executed under agentvfs-run with cwd on
the mount. Warm-up happens once (torch import); every experiment runs in a
generation forked from that warm template.

Boundary offers: a boundary with no pending snapshot returns immediately, so
offering one every 10ms while idle is cheap and makes the harness's blocking
snapshot request latch deterministically.

Fork-safety: warm-up must not run any OpenMP parallel region before the
template forks (import only); the resumed generation re-pins thread count
before running the experiment.
"""
import contextlib
import os
import runpy
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_boundary import boundary  # noqa: E402

RUN = Path(".sml-run")  # cwd is the mount


def main() -> None:
    # cwd is the mount; put it on sys.path so train.py's sibling imports
    # (`from prepare import ...`) resolve here, exactly as `python train.py`
    # would (runpy.run_path does NOT add the script's directory itself).
    sys.path.insert(0, os.getcwd())
    warm = os.environ.get("SML_WARM_IMPORT", "torch")
    if warm == "torch":
        import torch  # noqa: F401  (the warmth being templated)
    while True:
        while not (RUN / "go").exists():
            boundary("warm")
            time.sleep(0.01)
        if warm == "torch":
            import torch
            torch.set_num_threads(int(os.environ.get("OMP_NUM_THREADS", "4")))
        step = (RUN / "go").read_text().strip()
        (RUN / "go").unlink()
        out = RUN / "out.txt"
        with open(out, "w") as f, contextlib.redirect_stdout(f), \
                contextlib.redirect_stderr(f):
            try:
                runpy.run_path("train.py", run_name="__main__")
            except SystemExit as e:
                # parity with fresh mode, where rc!=0 fails the experiment
                if e.code not in (None, 0):
                    print(f"TRAINER-EXCEPTION: SystemExit({e.code!r})")
            except BaseException as e:  # a broken edit is an outcome
                print(f"TRAINER-EXCEPTION: {e!r}")
        (RUN / f"done-{step}").write_text("ok")


if __name__ == "__main__":
    main()
