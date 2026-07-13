import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import chess


class StateError(ValueError):
    pass


@dataclass(frozen=True)
class Position:
    fen: str
    turn: str
    legal_moves: tuple[str, ...]


@dataclass(frozen=True)
class GameState:
    schema: int
    initial_fen: str
    fen: str
    moves: tuple[str, ...]
    ply: int
    result: str

    @classmethod
    def initial(cls, initial_fen: str = chess.STARTING_FEN) -> "GameState":
        board = chess.Board(initial_fen)
        return cls(1, initial_fen, board.fen(), (), 0, board.result(claim_draw=True))

    @classmethod
    def from_dict(cls, data: dict) -> "GameState":
        if data.get("schema") != 1:
            raise StateError("unsupported game-state schema")
        state = cls(1, str(data["initial_fen"]), str(data["fen"]),
                    tuple(data["moves"]), int(data["ply"]), str(data["result"]))
        expected = replay(state.initial_fen, state.moves)
        if state.fen != expected.fen:
            raise StateError("fen does not match replay")
        if state.ply != expected.ply or state.result != expected.result:
            raise StateError("metadata does not match replay")
        return state

    @classmethod
    def load(cls, path: Path) -> "GameState":
        try:
            return cls.from_dict(json.loads(Path(path).read_text()))
        except (KeyError, TypeError, json.JSONDecodeError) as exc:
            raise StateError(f"invalid game state: {exc}") from exc

    def to_dict(self) -> dict:
        return {"schema": self.schema, "initial_fen": self.initial_fen,
                "fen": self.fen, "moves": list(self.moves), "ply": self.ply,
                "result": self.result}

    def save(self, path: Path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp",
                                    dir=path.parent)
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(self.to_dict(), stream, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(name, path)
        finally:
            try:
                os.unlink(name)
            except FileNotFoundError:
                pass

    def board(self) -> chess.Board:
        board = chess.Board(self.initial_fen)
        for text in self.moves:
            board.push(chess.Move.from_uci(text))
        return board

    def position(self) -> Position:
        board = self.board()
        return Position(board.fen(), "white" if board.turn else "black",
                        tuple(move.uci() for move in board.legal_moves))

    def apply(self, move_uci: str) -> "GameState":
        board = self.board()
        try:
            move = chess.Move.from_uci(move_uci)
        except ValueError as exc:
            raise StateError(f"invalid UCI move: {move_uci}") from exc
        if move not in board.legal_moves:
            raise StateError(f"illegal move: {move_uci}")
        board.push(move)
        return GameState(1, self.initial_fen, board.fen(),
                         self.moves + (move_uci,), self.ply + 1,
                         board.result(claim_draw=True))


def replay(initial_fen: str, moves: Sequence[str]) -> GameState:
    state = GameState.initial(initial_fen)
    for move in moves:
        state = state.apply(str(move))
    return state
