import json

import chess
import pytest

from src.game import GameState, StateError, replay


def test_initial_apply_save_load_and_replay(tmp_path):
    path = tmp_path / "game.json"
    state = GameState.initial().apply("e2e4").apply("e7e5")
    state.save(path)
    loaded = GameState.load(path)
    assert loaded == state
    assert loaded.moves == ("e2e4", "e7e5")
    assert loaded.ply == 2
    assert replay(chess.STARTING_FEN, loaded.moves) == loaded


def test_load_rejects_fen_that_disagrees_with_moves(tmp_path):
    path = tmp_path / "game.json"
    data = GameState.initial().apply("e2e4").to_dict()
    data["fen"] = chess.STARTING_FEN
    path.write_text(json.dumps(data))
    with pytest.raises(StateError, match="fen does not match replay"):
        GameState.load(path)


def test_apply_rejects_illegal_or_malformed_move():
    state = GameState.initial()
    with pytest.raises(StateError, match="illegal move"):
        state.apply("e7e5")
    with pytest.raises(StateError, match="invalid UCI"):
        state.apply("pawn-e4")


def test_save_uses_replace_and_leaves_no_temp_file(tmp_path):
    path = tmp_path / "game.json"
    GameState.initial().save(path)
    assert path.exists()
    assert list(tmp_path.glob(".game.json.*.tmp")) == []
