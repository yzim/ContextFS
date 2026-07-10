"""CPU overlay: exact anchored replacements applied to a SEEDED COPY of
vendor/autoresearch. Never applied to the vendored files themselves."""
from pathlib import Path


class OverlayDriftError(RuntimeError):
    pass


OVERLAY = [
    # --- train.py: remove CUDA flash-attention kernel loading ---
    ("train.py",
     '''from kernels import get_kernel
cap = torch.cuda.get_device_capability()
# varunneal's FA3 is Hopper only, use kernels-community on non-Hopper GPUs
repo = "varunneal/flash-attention-3" if cap == (9, 0) else "kernels-community/flash-attn3"
fa3 = get_kernel(repo).flash_attn_interface''',
     '''# CPU overlay: flash-attention replaced by SDPA (no-cuda-ok)''',
     1),
    # --- train.py: SDPA replacement honoring the banded window ---
    ("train.py",
     "        y = fa3.flash_attn_func(q, k, v, causal=True, window_size=window_size)",
     '''        Tq = q.shape[1]
        w = window_size[0] if window_size is not None else -1
        if w < 0 or w >= Tq:
            y = F.scaled_dot_product_attention(
                q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2),
                is_causal=True).transpose(1, 2)
        else:
            i = torch.arange(Tq, device=q.device)
            j = torch.arange(k.shape[1], device=k.device)
            m = (j[None, :] <= i[:, None]) & (j[None, :] >= i[:, None] - w)
            y = F.scaled_dot_product_attention(
                q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2),
                attn_mask=m).transpose(1, 2)''',
     1),
    # --- train.py: disable torch.compile (slow/fragile on CPU) ---
    ("train.py", "@torch.compile(dynamic=False, fullgraph=True)\n", "", 2),
    ("train.py", "model = torch.compile(model, dynamic=False)",
     "# CPU overlay: torch.compile disabled", 1),
    # --- train.py: device + autocast + seeds + vram ---
    ("train.py", "torch.cuda.manual_seed(42)", "torch.manual_seed(42)", 1),
    ("train.py", 'device = torch.device("cuda")', 'device = torch.device("cpu")', 1),
    # keep autocast, retargeted to CPU: upstream stores embeddings/rotary in
    # bf16 (train.py wte/ve/cos-sin casts) and counts on autocast to downcast
    # the fp32 Linear weights during matmul — a nullcontext crashes with
    # "expected m1 and m2 to have the same dtype" at the first projection
    ("train.py",
     'autocast_ctx = torch.amp.autocast(device_type="cuda", dtype=torch.bfloat16)',
     'autocast_ctx = torch.amp.autocast(device_type="cpu", dtype=torch.bfloat16)', 1),
    ("train.py", "peak_vram_mb = torch.cuda.max_memory_allocated() / 1024 / 1024",
     "peak_vram_mb = 0.0", 1),
    # --- train.py: CUDA-only device synchronize calls are no-ops on CPU ---
    ("train.py", "    torch.cuda.synchronize()",
     "    pass  # CPU overlay: device synchronize is a no-op on CPU", 2),
    # --- train.py: scale the model down (autoresearch README small-platform knobs) ---
    ("train.py", "TOTAL_BATCH_SIZE = 2**19", "TOTAL_BATCH_SIZE = 2**14", 1),
    ("train.py", "DEPTH = 8", "DEPTH = 4", 1),
    ("train.py", "DEVICE_BATCH_SIZE = 128", "DEVICE_BATCH_SIZE = 8", 1),
    # --- prepare.py: budget from env, smaller eval/context, CPU buffers ---
    ("prepare.py", "TIME_BUDGET = 300",
     'TIME_BUDGET = int(os.environ.get("TRAIN_BUDGET_S", "60"))', 1),
    ("prepare.py", "MAX_SEQ_LEN = 2048", "MAX_SEQ_LEN = 256", 1),
    ("prepare.py", "EVAL_TOKENS = 40 * 524288", "EVAL_TOKENS = 4 * 16384", 1),
    ("prepare.py", "pin_memory=True", "pin_memory=False", 1),
    ("prepare.py", 'gpu_buffer = torch.empty(2 * B * T, dtype=torch.long, device="cuda")',
     'gpu_buffer = torch.empty(2 * B * T, dtype=torch.long, device="cpu")', 1),
    ("prepare.py", 'token_bytes = get_token_bytes(device="cuda")',
     'token_bytes = get_token_bytes(device="cpu")', 1),
]


def apply_overlay(tree: Path) -> None:
    for fname, old, new, count in OVERLAY:
        p = Path(tree) / fname
        text = p.read_text()
        found = text.count(old)
        if found != count:
            raise OverlayDriftError(
                f"{fname}: pattern occurs {found}x, expected {count}x. "
                f"Upstream autoresearch changed; re-anchor this entry:\n{old[:120]}")
        p.write_text(text.replace(old, new))
