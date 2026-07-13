"""Stateless model adapters, strict UCI parsing, and result records.

The OpenAI adapters are stateless: every attempt sends a fresh system/user
message pair derived only from the current :class:`~src.game.Position`. No
prior moves, board history, or conversation is ever included, so an actor or
speculator call can be retried, replayed, or run in parallel without hidden
state. Parsing is intentionally strict: malformed or illegal tokens are never
silently repaired (a promotion suffix is never auto-completed).
"""

import re
import time
from dataclasses import dataclass
from typing import Protocol

from src.game import Position

# A UCI move is four square characters with an optional promotion suffix.
# Lowercase only; uppercase tokens are treated as malformed.
_UCI_RE = re.compile(r"[a-h][1-8][a-h][1-8][qrbn]?")
_BRACKET_RE = re.compile(r"\[\s*([^\[\]]*?)\s*\]")

_RAW_EXCERPT_CAP = 4096


class LLMOutputError(Exception):
    """Raised when a model response cannot be turned into a legal move set.

    ``raw_excerpt`` carries at most ``_RAW_EXCERPT_CAP`` characters of the
    offending model output so a runner can write a bounded diagnostic to disk
    (``results/invalid-<role>-ply<N>.txt``) without dragging whole responses or
    any request material off the mount. Transport failures carry an empty
    excerpt; the API key and request headers are never placed on this error.
    """

    def __init__(self, message: str, *, role: str = "",
                 attempts: int = 0, raw_excerpt: str = "",
                 latency_s: float = 0.0, tokens_in: int = 0,
                 tokens_out: int = 0):
        super().__init__(message)
        self.role = role
        self.attempts = attempts
        self.raw_excerpt = raw_excerpt[:_RAW_EXCERPT_CAP]
        self.latency_s = latency_s
        self.tokens_in = tokens_in
        self.tokens_out = tokens_out


@dataclass(frozen=True)
class MoveResult:
    move: str
    latency_s: float
    tokens_in: int = 0
    tokens_out: int = 0


@dataclass(frozen=True)
class PredictionResult:
    moves: tuple[str, ...]
    latency_s: float
    tokens_in: int = 0
    tokens_out: int = 0


class Actor(Protocol):
    def choose_move(self, position: Position) -> MoveResult: ...


class Speculator(Protocol):
    def predict_moves(self, position: Position, k: int) -> PredictionResult: ...


def _extract_tokens(text: str) -> list[str]:
    """Yield candidate move tokens from bracketed groups or bare whitespace.

    Bracketed ``[token]`` groups take priority when present (the prompt format
    asks for brackets); otherwise the raw text is split on commas/whitespace so
    a bare multi-token response is still detected. Each candidate is stripped.
    """
    bracketed = _BRACKET_RE.findall(text)
    if bracketed:
        raw = bracketed
    else:
        raw = re.split(r"[\s,]+", text.strip())
    return [piece.strip() for piece in raw if piece and piece.strip()]


def _is_well_formed(token: str) -> bool:
    return _UCI_RE.fullmatch(token) is not None


def parse_actor_move(text: str, legal_moves: tuple[str, ...]) -> str:
    """Return the single legal UCI move encoded in ``text``.

    Raises :class:`LLMOutputError` with "exactly one" wording when more than one
    legal move is present, "legal" wording when a token is well-formed but not
    legal, and "invalid" wording when no token is well-formed.
    """
    legal = set(legal_moves)
    tokens = _extract_tokens(text)
    legal_hits = [t for t in tokens if t in legal]

    if len(legal_hits) == 1:
        return legal_hits[0]
    if len(legal_hits) >= 2:
        raise LLMOutputError(
            "actor output yielded multiple legal moves; expected exactly one",
            role="actor", raw_excerpt=text)
    if not tokens:
        raise LLMOutputError(
            "actor output contained no move; expected exactly one legal UCI token",
            role="actor", raw_excerpt=text)
    if any(_is_well_formed(t) for t in tokens):
        raise LLMOutputError(
            "actor output is not a legal move in this position",
            role="actor", raw_excerpt=text)
    raise LLMOutputError(
        "actor output is an invalid UCI token; no parseable move",
        role="actor", raw_excerpt=text)


def parse_predictions(text: str, legal_moves: tuple[str, ...],
                      k: int) -> tuple[str, ...]:
    """Return up to ``k`` distinct legal UCI moves, in first-seen order.

    Malformed tokens, illegal moves, and duplicates are silently discarded;
    order is preserved. At most ``k`` moves are returned.
    """
    legal = set(legal_moves)
    tokens = _extract_tokens(text)
    seen: set[str] = set()
    result: list[str] = []
    for token in tokens:
        if not _is_well_formed(token) or token not in legal or token in seen:
            continue
        seen.add(token)
        result.append(token)
        if len(result) >= k:
            break
    return tuple(result)


def build_position_prompt(position: Position) -> str:
    return ("Choose one legal chess move. Return only [UCI_MOVE].\n"
            f"Side to move: {position.turn}\n"
            f"FEN: {position.fen}\n"
            "Legal UCI moves: " + ", ".join(position.legal_moves))


def build_prediction_prompt(position: Position, k: int) -> str:
    return (f"Predict the next {k} legal chess moves for the side to move, in "
            "order. Return only comma-separated [UCI_MOVE] tokens.\n"
            f"Side to move: {position.turn}\n"
            f"FEN: {position.fen}\n"
            "Legal UCI moves: " + ", ".join(position.legal_moves))


_SYSTEM = ("You are a chess move generator. Respond only with the requested "
           "UCI move tokens inside square brackets and nothing else.")


def _usage(response) -> tuple[int, int]:
    usage = getattr(response, "usage", None)
    tokens_in = getattr(usage, "prompt_tokens", 0) or 0
    tokens_out = getattr(usage, "completion_tokens", 0) or 0
    return tokens_in, tokens_out


def _content(response) -> str:
    """Extract the model text from a chat-completions response.

    A degenerate response (no ``choices``, or ``content is None`` such as a
    content-filter finish reason) yields an empty string, so the caller's
    retry loop treats it like any other malformed response and ultimately
    raises :class:`LLMOutputError` -- never a bare ``IndexError`` /
    ``TypeError``. A degenerate response carries no model text, so the
    resulting excerpt is empty, distinct from a malformed-but-non-empty
    response whose excerpt carries the capped model text.
    """
    choices = getattr(response, "choices", None) or []
    if not choices:
        return ""
    message = getattr(choices[0], "message", None)
    content = getattr(message, "content", None)
    return content or ""


class OpenAIActor:
    """Stateless actor over an injected OpenAI-style chat-completions client."""

    def __init__(self, client, model: str, timeout_s: float = 30.0,
                 retries: int = 2):
        self._client = client
        self._model = model
        self._timeout_s = timeout_s
        self._retries = retries

    def choose_move(self, position: Position) -> MoveResult:
        user_prompt = build_position_prompt(position)
        attempts = 0
        total_latency_s = 0.0
        total_tokens_in = 0
        total_tokens_out = 0
        last_error: LLMOutputError | None = None
        for _ in range(self._retries + 1):
            attempts += 1
            messages = [
                {"role": "system", "content": _SYSTEM},
                {"role": "user", "content": user_prompt},
            ]
            start = time.perf_counter()
            try:
                response = self._client.chat.completions.create(
                    model=self._model, messages=messages,
                    timeout=self._timeout_s)
            except Exception as exc:  # transport / API failure
                total_latency_s += time.perf_counter() - start
                last_error = LLMOutputError(
                    f"actor transport error after {attempts} attempt(s): "
                    f"{type(exc).__name__}",
                    role="actor", attempts=attempts, raw_excerpt="",
                    latency_s=total_latency_s, tokens_in=total_tokens_in,
                    tokens_out=total_tokens_out)
                continue
            total_latency_s += time.perf_counter() - start
            content = _content(response)
            tokens_in, tokens_out = _usage(response)
            total_tokens_in += tokens_in
            total_tokens_out += tokens_out
            try:
                move = parse_actor_move(content, position.legal_moves)
            except LLMOutputError as parse_error:
                last_error = LLMOutputError(
                    f"actor output rejected after {attempts} attempt(s): "
                    f"{parse_error}",
                    role="actor", attempts=attempts, raw_excerpt=content,
                    latency_s=total_latency_s, tokens_in=total_tokens_in,
                    tokens_out=total_tokens_out)
                continue
            return MoveResult(move=move, latency_s=total_latency_s,
                              tokens_in=total_tokens_in,
                              tokens_out=total_tokens_out)
        assert last_error is not None
        raise last_error


class OpenAISpeculator:
    """Stateless speculator over an injected OpenAI-style chat-completions client."""

    def __init__(self, client, model: str, timeout_s: float = 30.0,
                 retries: int = 2):
        self._client = client
        self._model = model
        self._timeout_s = timeout_s
        self._retries = retries

    def predict_moves(self, position: Position, k: int) -> PredictionResult:
        user_prompt = build_prediction_prompt(position, k)
        attempts = 0
        total_latency_s = 0.0
        total_tokens_in = 0
        total_tokens_out = 0
        last_error: LLMOutputError | None = None
        for _ in range(self._retries + 1):
            attempts += 1
            messages = [
                {"role": "system", "content": _SYSTEM},
                {"role": "user", "content": user_prompt},
            ]
            start = time.perf_counter()
            try:
                response = self._client.chat.completions.create(
                    model=self._model, messages=messages,
                    timeout=self._timeout_s)
            except Exception as exc:  # transport / API failure
                total_latency_s += time.perf_counter() - start
                last_error = LLMOutputError(
                    f"speculator transport error after {attempts} attempt(s): "
                    f"{type(exc).__name__}",
                    role="speculator", attempts=attempts, raw_excerpt="",
                    latency_s=total_latency_s, tokens_in=total_tokens_in,
                    tokens_out=total_tokens_out)
                continue
            total_latency_s += time.perf_counter() - start
            content = _content(response)
            tokens_in, tokens_out = _usage(response)
            total_tokens_in += tokens_in
            total_tokens_out += tokens_out
            moves = parse_predictions(content, position.legal_moves, k)
            if moves:
                return PredictionResult(moves=moves, latency_s=total_latency_s,
                                         tokens_in=total_tokens_in,
                                         tokens_out=total_tokens_out)
            last_error = LLMOutputError(
                f"speculator output yielded no legal moves after "
                f"{attempts} attempt(s)",
                role="speculator", attempts=attempts, raw_excerpt=content,
                latency_s=total_latency_s, tokens_in=total_tokens_in,
                tokens_out=total_tokens_out)
        assert last_error is not None
        raise last_error
