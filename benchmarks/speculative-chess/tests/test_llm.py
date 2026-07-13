from types import SimpleNamespace

import pytest

from src.game import GameState
from src.llm import (LLMOutputError, OpenAIActor, OpenAISpeculator,
                     build_position_prompt, parse_actor_move, parse_predictions)


LEGAL = GameState.initial().position().legal_moves


def test_actor_parser_requires_exactly_one_legal_uci_move():
    assert parse_actor_move("[e2e4]", LEGAL) == "e2e4"
    with pytest.raises(LLMOutputError, match="exactly one"):
        parse_actor_move("e2e4 and d2d4", LEGAL)
    with pytest.raises(LLMOutputError, match="legal"):
        parse_actor_move("[e7e5]", LEGAL)


def test_prediction_parser_preserves_order_deduplicates_and_caps_k():
    text = "[d2d4], [e2e4], [d2d4], [g1f3]"
    assert parse_predictions(text, LEGAL, 2) == ("d2d4", "e2e4")


def test_prompt_contains_only_position_contract():
    prompt = build_position_prompt(GameState.initial().position())
    assert "Side to move: white" in prompt
    assert "Legal UCI moves:" in prompt
    assert "history" not in prompt.lower()


def test_openai_actor_reports_move_tokens_and_latency(monkeypatch):
    response = SimpleNamespace(
        choices=[SimpleNamespace(message=SimpleNamespace(content="[e2e4]"))],
        usage=SimpleNamespace(prompt_tokens=11, completion_tokens=3))
    completions = SimpleNamespace(create=lambda **kwargs: response)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    times = iter([1.0, 1.25])
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: next(times))
    actor = OpenAIActor(client=client, model="actor-model", timeout_s=20, retries=1)
    result = actor.choose_move(GameState.initial().position())
    assert result.move == "e2e4"
    assert (result.tokens_in, result.tokens_out) == (11, 3)


def test_actor_retry_accumulates_latency_and_tokens(monkeypatch):
    responses = iter([
        SimpleNamespace(
            choices=[SimpleNamespace(message=SimpleNamespace(content="bad"))],
            usage=SimpleNamespace(prompt_tokens=11, completion_tokens=3)),
        SimpleNamespace(
            choices=[SimpleNamespace(message=SimpleNamespace(content="[e2e4]"))],
            usage=SimpleNamespace(prompt_tokens=13, completion_tokens=2)),
    ])
    completions = SimpleNamespace(create=lambda **kwargs: next(responses))
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    times = iter([1.0, 1.2, 1.3, 1.5])
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: next(times))

    result = OpenAIActor(client=client, model="actor-model", retries=1).choose_move(
        GameState.initial().position())

    assert result.latency_s == pytest.approx(0.4)
    assert (result.tokens_in, result.tokens_out) == (24, 5)


# --- Additional offline contract tests: strictness and sanitization. ---


def test_actor_parser_rejects_malformed_token_as_invalid():
    with pytest.raises(LLMOutputError, match="invalid"):
        parse_actor_move("[zzzz]", LEGAL)


def test_prediction_parser_discards_malformed_illegal_and_duplicates():
    # "junk" is malformed; e7e5 is illegal for white at the start position.
    text = "[d2d4], junk, [e7e5], [g1f3], [d2d4]"
    assert parse_predictions(text, LEGAL, 5) == ("d2d4", "g1f3")


def test_raw_excerpt_capped_at_4_kib():
    text = "q" * 5120  # 5 KiB: no brackets, no legal UCI token.
    with pytest.raises(LLMOutputError) as exc_info:
        parse_actor_move(text, LEGAL)
    assert len(exc_info.value.raw_excerpt) == 4096


def test_transport_error_excerpt_empty_and_free_of_secrets(monkeypatch):
    secret = "sk-SECRETKEY-DO-NOT-LEAK"

    def raise_with_secret(**kwargs):
        raise RuntimeError(f"Authorization: Bearer {secret}; x-custom: leak")

    completions = SimpleNamespace(create=raise_with_secret)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: 0.0)
    actor = OpenAIActor(client=client, model="actor-model", timeout_s=5, retries=0)
    with pytest.raises(LLMOutputError) as exc_info:
        actor.choose_move(GameState.initial().position())
    err = exc_info.value
    assert err.raw_excerpt == ""
    assert err.attempts == 1
    assert err.role == "actor"
    assert secret not in (str(err) + repr(err))


def test_openai_speculator_reports_moves_tokens_and_latency(monkeypatch):
    response = SimpleNamespace(
        choices=[SimpleNamespace(message=SimpleNamespace(
            content="[d2d4], [e2e4], [g1f3]"))],
        usage=SimpleNamespace(prompt_tokens=24, completion_tokens=7))
    completions = SimpleNamespace(create=lambda **kwargs: response)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    times = iter([2.0, 2.5])
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: next(times))
    speculator = OpenAISpeculator(client=client, model="spec-model",
                                  timeout_s=20, retries=1)
    result = speculator.predict_moves(GameState.initial().position(), k=3)
    assert result.moves == ("d2d4", "e2e4", "g1f3")
    assert (result.tokens_in, result.tokens_out) == (24, 7)
    assert result.latency_s == pytest.approx(0.5)


def test_speculator_retry_accumulates_latency_and_tokens(monkeypatch):
    responses = iter([
        SimpleNamespace(
            choices=[SimpleNamespace(message=SimpleNamespace(content="bad"))],
            usage=SimpleNamespace(prompt_tokens=7, completion_tokens=1)),
        SimpleNamespace(
            choices=[SimpleNamespace(message=SimpleNamespace(content="[e2e4]"))],
            usage=SimpleNamespace(prompt_tokens=9, completion_tokens=2)),
    ])
    completions = SimpleNamespace(create=lambda **kwargs: next(responses))
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    times = iter([2.0, 2.1, 2.2, 2.4])
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: next(times))

    result = OpenAISpeculator(client=client, model="spec", retries=1).predict_moves(
        GameState.initial().position(), k=1)

    assert result.latency_s == pytest.approx(0.3)
    assert (result.tokens_in, result.tokens_out) == (16, 3)


# --- Hardening: empty-after-discard error path and degenerate responses. ---


def test_speculator_all_malformed_raises_with_capped_excerpt(monkeypatch):
    # All tokens malformed -> parse_predictions returns () -> empty-after-discard
    # must retry then raise LLMOutputError carrying the capped model text.
    garbage = "x" * 5120  # longer than the 4 KiB cap; every token is malformed
    response = SimpleNamespace(
        choices=[SimpleNamespace(message=SimpleNamespace(content=garbage))],
        usage=SimpleNamespace(prompt_tokens=10, completion_tokens=5))
    completions = SimpleNamespace(create=lambda **kwargs: response)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: 0.0)
    speculator = OpenAISpeculator(client=client, model="spec-model",
                                  timeout_s=5, retries=0)
    with pytest.raises(LLMOutputError) as exc_info:
        speculator.predict_moves(GameState.initial().position(), k=3)
    err = exc_info.value
    assert err.role == "speculator"
    assert err.attempts == 1
    assert len(err.raw_excerpt) == 4096
    assert err.raw_excerpt == garbage[:4096]
    assert (err.tokens_in, err.tokens_out) == (10, 5)


def test_actor_no_choices_raises_llm_output_error(monkeypatch):
    # Degenerate response (zero choices) must become LLMOutputError, not IndexError.
    response = SimpleNamespace(choices=[], usage=None)
    completions = SimpleNamespace(create=lambda **kwargs: response)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: 0.0)
    actor = OpenAIActor(client=client, model="actor-model", timeout_s=5, retries=0)
    with pytest.raises(LLMOutputError) as exc_info:
        actor.choose_move(GameState.initial().position())
    err = exc_info.value
    assert err.role == "actor"
    assert err.attempts == 1
    assert err.raw_excerpt == ""  # degenerate response carries no model text


def test_speculator_none_content_raises_llm_output_error(monkeypatch):
    # content=None (e.g. content-filter finish reason) must not feed None into
    # the parser; it is treated as a malformed response with an empty excerpt.
    response = SimpleNamespace(
        choices=[SimpleNamespace(message=SimpleNamespace(content=None))],
        usage=SimpleNamespace(prompt_tokens=8, completion_tokens=0))
    completions = SimpleNamespace(create=lambda **kwargs: response)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    monkeypatch.setattr("src.llm.time.perf_counter", lambda: 0.0)
    speculator = OpenAISpeculator(client=client, model="spec-model",
                                  timeout_s=5, retries=0)
    with pytest.raises(LLMOutputError) as exc_info:
        speculator.predict_moves(GameState.initial().position(), k=3)
    err = exc_info.value
    assert err.role == "speculator"
    assert err.attempts == 1
    assert err.raw_excerpt == ""
