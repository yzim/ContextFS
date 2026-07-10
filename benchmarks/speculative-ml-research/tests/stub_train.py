"""Stub trainer: reads ./train.py, sleeps STUB_SLEEP_S, prints a deterministic
val_bpb derived from the train.py content. No torch needed."""
import hashlib
import os
import time

src = open("train.py").read()
time.sleep(float(os.environ.get("STUB_SLEEP_S", "2")))
if "CRASH" in src:  # simulate a broken/overweight edit: trainer dies, no metric
    raise SystemExit(1)
digest = int(hashlib.sha256(src.encode()).hexdigest(), 16)
base = 4.0 - (digest % 1000) / 10000.0          # deterministic in content
if "REGRESS" in src:
    base += 0.5
budget = os.environ.get("TRAIN_BUDGET_S", "60")
print("---")
print(f"val_bpb:          {base:.6f}")
print(f"training_seconds: {float(budget):.1f}")
print(f"total_seconds:    {float(budget):.1f}")
