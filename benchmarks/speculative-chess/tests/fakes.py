"""Reusable scripted actor/speculator fakes for offline benchmark tests.

These replay explicit result sequences so later tasks can drive the harness
without any network or model. Filesystem, AgentVFS, and candidate fakes belong
in the test module that owns them; this file intentionally provides only the
two model-side fakes.

``delay_s`` on the convenience constructors doubles as a REAL ``time.sleep`` so
the speculative runner's bounded two-future race is deterministic: a smaller
speculator delay lets the speculator finish first (a hit window), a smaller
actor delay lets the actor finish first (a missed window). Direct construction
(:meth:`ScriptedActor(results=...)`) passes real model :class:`MoveResult`
records and does NOT sleep, so unit tests that only need a canned latency stay
instant.
"""

import time
from collections.abc import Iterable

from src.game import Position
from src.llm import MoveResult, PredictionResult


class ScriptedActor:
    """Replays :class:`MoveResult` records in order via ``choose_move``.

    Build with :meth:`moves` from plain UCI strings, or pass pre-built
    :class:`MoveResult` records to the constructor for explicit control. The
    script raises :class:`IndexError` once exhausted so an under-provisioned
    test fails loudly instead of looping forever.
    """

    def __init__(self, results: Iterable[MoveResult], sleep_s: float = 0.0):
        self._results = tuple(results)
        self._sleep_s = sleep_s
        self._index = 0

    @classmethod
    def moves(cls, moves: Iterable[str], delay_s: float = 0.0) -> "ScriptedActor":
        return cls((MoveResult(move=m, latency_s=delay_s) for m in moves),
                   sleep_s=delay_s)

    def choose_move(self, position: Position) -> MoveResult:
        if self._index >= len(self._results):
            raise IndexError("ScriptedActor script exhausted")
        result = self._results[self._index]
        self._index += 1
        if self._sleep_s:
            time.sleep(self._sleep_s)
        return result


class ScriptedSpeculator:
    """Replays :class:`PredictionResult` records in order via ``predict_moves``.

    Build with :meth:`predictions` from move strings or tuples of moves, or
    pass pre-built :class:`PredictionResult` records to the constructor.
    """

    def __init__(self, results: Iterable[PredictionResult], sleep_s: float = 0.0):
        self._results = tuple(results)
        self._sleep_s = sleep_s
        self._index = 0

    @classmethod
    def predictions(cls, predictions: Iterable, delay_s: float = 0.0) -> "ScriptedSpeculator":
        records = []
        for entry in predictions:
            moves = (entry,) if isinstance(entry, str) else tuple(entry)
            records.append(PredictionResult(moves=moves, latency_s=delay_s))
        return cls(records, sleep_s=delay_s)

    def predict_moves(self, position: Position, k: int) -> PredictionResult:
        if self._index >= len(self._results):
            raise IndexError("ScriptedSpeculator script exhausted")
        result = self._results[self._index]
        self._index += 1
        if self._sleep_s:
            time.sleep(self._sleep_s)
        return result
