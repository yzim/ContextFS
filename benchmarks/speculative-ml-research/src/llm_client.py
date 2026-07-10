"""Actor/Speculator over OpenAI-compatible APIs, plus scripted fakes for tests."""
import difflib
import io
import re
import time
import tokenize
from dataclasses import dataclass


@dataclass
class Decision:
    keep: bool
    new_train_py: str
    latency_s: float = 0.0
    tokens_in: int = 0
    tokens_out: int = 0


@dataclass
class Guess:
    new_train_py: str
    confidence: float


@dataclass
class Speculation:
    guesses: list
    latency_s: float = 0.0
    tokens_in: int = 0
    tokens_out: int = 0
    apply_failures: int = 0


def normalize_python(src: str) -> str:
    try:
        toks = [(t.type, t.string) for t in tokenize.generate_tokens(io.StringIO(src).readline)
                if t.type not in (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE,
                                  tokenize.INDENT, tokenize.DEDENT)]
        return " ".join(f"{t}:{s}" for t, s in toks)
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return "\n".join(l.strip() for l in src.splitlines() if l.strip())


def edits_match(a: str, b: str) -> bool:
    return normalize_python(a) == normalize_python(b)


def edit_diff(old: str, new: str) -> str:
    """Compact unified diff (1 context line, no file headers) — the format
    past actor edits are shown to the speculator in."""
    lines = difflib.unified_diff(old.splitlines(), new.splitlines(),
                                 n=1, lineterm="")
    return "\n".join(l for l in lines if not l.startswith(("---", "+++")))


FENCE_RE = re.compile(r"```python\n(.*?)```", re.DOTALL)
CONF_RE = re.compile(r"CONFIDENCE:\s*([0-9.]+)")
BLOCK_RE = re.compile(r"<<<<<<< SEARCH\n(.*?)\n=======\n(.*?)>>>>>>> REPLACE",
                      re.DOTALL)


@dataclass
class EditGuess:
    blocks: list  # of (search, replace) pairs
    confidence: float


def apply_edit_blocks(text: str, blocks: list) -> str | None:
    """Apply anchored SEARCH/REPLACE blocks in order; None if any anchor is
    missing or the candidate has no blocks (an edit must change something)."""
    if not blocks:
        return None
    for search, replace in blocks:
        if not search or search not in text:
            return None
        text = text.replace(search, replace, 1)
    return text


def parse_edit_guesses(text: str) -> list:
    """Split a speculator response into CONFIDENCE-headed candidates, each
    carrying its SEARCH/REPLACE blocks; blockless candidates are dropped."""
    guesses = []
    parts = CONF_RE.split(text)  # [pre, conf1, seg1, conf2, seg2, ...]
    for conf, seg in zip(parts[1::2], parts[2::2]):
        # the REPLACE side keeps its trailing newline from DOTALL up to the
        # marker line; strip exactly one to mirror the SEARCH side's \n anchor
        blocks = [(s, r[:-1] if r.endswith("\n") else r)
                  for s, r in BLOCK_RE.findall(seg)]
        if blocks:
            guesses.append(EditGuess(blocks, float(conf)))
    guesses.sort(key=lambda g: -g.confidence)
    return guesses


def parse_decision(text: str):
    # take the FIRST decision marker: searching for "keep" anywhere would
    # false-positive on a discard response whose code mentions the word
    m = re.search(r"DECISION:\s*(keep|discard)", text, re.IGNORECASE)
    keep = bool(m and m.group(1).lower() == "keep")
    m = FENCE_RE.search(text)
    if not m:
        raise ValueError("Actor response has no ```python block")
    return keep, m.group(1).strip()


def parse_guesses(text: str):
    guesses = []
    confs = CONF_RE.findall(text)
    blocks = FENCE_RE.findall(text)
    for i, block in enumerate(blocks):
        conf = float(confs[i]) if i < len(confs) else 0.0
        guesses.append(Guess(block.strip(), conf))
    guesses.sort(key=lambda g: -g.confidence)
    return guesses


ACTOR_SYSTEM = """You are an ML research agent improving a small GPT training
script (train.py). You run experiments: each experiment edits train.py and
trains for a fixed wall-clock budget; lower val_bpb is better.
Given the experiment history and the current train.py, respond with EXACTLY:
Line 1: `DECISION: keep` if the most recent experiment improved val_bpb over
the best previous kept experiment, else `DECISION: discard`.
Then ONE fenced ```python block containing the COMPLETE next train.py to try
(one focused change: a hyperparameter, schedule, or small architecture tweak).
The environment is CPU-only with a strict wall-clock training budget: never
increase model size (n_layer/n_embd/n_head/sequence_len) — a single optimizer
step must stay well under the budget. A "failed" history entry means that
experiment crashed or timed out; do not retry it.
No other prose."""

SPECULATOR_SYSTEM = """A senior ML research agent (the Actor) is about to
decide the next edit to train.py given the experiment history. Predict the
Actor's next edit. Reason very very briefly — do not deliberate; return the
most likely candidates fast. Respond with {k} candidates, highest-confidence
first, each as a line `CONFIDENCE: <0..1>` followed by one or more edit blocks
in EXACTLY this format:
<<<<<<< SEARCH
<lines copied verbatim from the current train.py>
=======
<replacement lines>
>>>>>>> REPLACE
The SEARCH text must match the current train.py exactly (whitespace included).
Keep edits minimal: one focused change per candidate (a hyperparameter,
schedule, or small architecture tweak). No other prose."""


def _history_text(history):
    lines = ["step,val_bpb,decision"]
    lines += [f"{h['step']},"
              + ("failed" if h["val_bpb"] is None else f"{h['val_bpb']:.6f}")
              + f",{h['decision']}" for h in history]
    return "\n".join(lines)


def _user_text(history, train_py: str) -> str:
    # shared by Actor and Speculator: the Speculator predicts the Actor's
    # output, so any drift between their contexts silently costs hit rate
    return (f"History:\n{_history_text(history)}\n\n"
            f"Current train.py:\n```python\n{train_py}\n```")


class _OpenAIChat:
    def __init__(self, model: str, base_url: str, api_key: str, timeout_s: int,
                 max_tokens: int | None = None):
        from openai import OpenAI
        self.client = OpenAI(base_url=base_url, api_key=api_key, timeout=timeout_s)
        self.model = model
        self.max_tokens = max_tokens

    def chat(self, system: str, user: str):
        # responses must contain a COMPLETE train.py; provider default output
        # caps (e.g. 4k) silently truncate the fenced block, so let the config
        # raise the ceiling where the provider allows it
        extra = {"max_tokens": self.max_tokens} if self.max_tokens else {}
        t0 = time.time()
        r = self.client.chat.completions.create(
            model=self.model, temperature=0.0,
            messages=[{"role": "system", "content": system},
                      {"role": "user", "content": user}], **extra)
        u = r.usage
        return (r.choices[0].message.content or "", time.time() - t0,
                u.prompt_tokens if u else 0, u.completion_tokens if u else 0)


class Actor(_OpenAIChat):
    def decide(self, history, train_py: str) -> Decision:
        text, dt, tin, tout = self.chat(ACTOR_SYSTEM, _user_text(history, train_py))
        keep, body = parse_decision(text)
        return Decision(keep, body, dt, tin, tout)


class Speculator(_OpenAIChat):
    def predict(self, history, train_py: str, k: int,
                edit_history=None) -> Speculation:
        user = _user_text(history, train_py)
        if edit_history:
            # the actor's editing style is the strongest predictor of its
            # next edit; the metric table alone forces re-inferring it
            user += ("\n\nThe Actor's previous edits (unified diffs, oldest "
                     "first):\n" + "\n\n".join(edit_history))
        text, dt, tin, tout = self.chat(SPECULATOR_SYSTEM.replace("{k}", str(k)),
                                        user)
        guesses, failures, seen = [], 0, set()
        for eg in parse_edit_guesses(text)[:k]:
            applied = apply_edit_blocks(train_py, eg.blocks)
            if applied is None:
                failures += 1
                continue
            key = normalize_python(applied)
            if key in seen:  # same materialized edit stated twice
                continue
            seen.add(key)
            guesses.append(Guess(applied, eg.confidence))
        return Speculation(guesses, dt, tin, tout, apply_failures=failures)


class FakeActor:
    def __init__(self, script, latency_s: float = 0.0):
        self.script = list(script)
        self.latency_s = latency_s
        self.i = 0

    def decide(self, history, train_py: str) -> Decision:
        time.sleep(self.latency_s)
        keep, body = self.script[self.i]
        self.i += 1
        return Decision(keep, body, self.latency_s, 0, 0)


class FakeSpeculator:
    def __init__(self, script, latency_s: float = 0.0, fail_steps=frozenset()):
        self.script = list(script)
        self.latency_s = latency_s
        self.fail_steps = set(fail_steps)
        self.i = 0

    def predict(self, history, train_py: str, k: int,
                edit_history=None) -> Speculation:
        step = self.i
        self.i += 1
        if step in self.fail_steps:
            raise TimeoutError(f"scripted speculator failure at step {step}")
        time.sleep(self.latency_s)
        return Speculation(self.script[step][:k], self.latency_s, 0, 0)
