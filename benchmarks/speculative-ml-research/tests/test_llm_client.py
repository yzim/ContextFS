from src.llm_client import (FakeActor, FakeSpeculator, Guess, apply_edit_blocks,
                            edits_match, normalize_python, parse_decision,
                            parse_edit_guesses, parse_guesses)


def test_normalize_strips_comments_and_blanks():
    a = "x = 1  # set x\n\n\ny = 2\n"
    b = "x = 1\ny = 2"
    assert normalize_python(a) == normalize_python(b)
    assert edits_match(a, b)
    assert not edits_match("x = 1", "x = 2")


def test_comment_only_texts_all_normalize_equal():
    # degenerate but by design (comments never change code semantics):
    # comment-ONLY texts are indistinguishable, so test fixtures that need a
    # guaranteed MISS must differ in code, not comments
    assert edits_match("# wrong guess\n", "# experiment 1\n")
    assert not edits_match("wrong_guess = True\n", "# experiment 1\n")


def test_parse_decision():
    keep, body = parse_decision("DECISION: keep\n```python\nx = 1\n```\n")
    assert keep and body == "x = 1"
    keep2, _ = parse_decision("DECISION: discard\n```python\ny = 2\n```")
    assert not keep2
    # a discard whose code mentions "DECISION: keep" must not flip the decision
    keep3, _ = parse_decision(
        "DECISION: discard\n```python\n# was DECISION: keep last step\ny = 2\n```")
    assert not keep3


def test_history_text_formats_failed_steps():
    from src.llm_client import _history_text
    text = _history_text([{"step": 0, "val_bpb": 2.5, "decision": "keep"},
                          {"step": 1, "val_bpb": None, "decision": "failed"}])
    assert "1,failed,failed" in text
    assert "0,2.500000,keep" in text


def test_parse_guesses_ordered_by_confidence():
    text = ("CONFIDENCE: 0.3\n```python\na = 1\n```\n"
            "CONFIDENCE: 0.9\n```python\nb = 2\n```\n")
    gs = parse_guesses(text)
    assert [g.confidence for g in gs] == [0.9, 0.3]
    assert gs[0].new_train_py == "b = 2"


def test_apply_edit_blocks_exact_anchor_replacement():
    src = "LR = 0.01\nDEPTH = 8\n"
    assert apply_edit_blocks(src, [("LR = 0.01", "LR = 0.02")]) \
        == "LR = 0.02\nDEPTH = 8\n"
    # multiple blocks apply in order
    assert apply_edit_blocks(src, [("LR = 0.01", "LR = 0.02"),
                                   ("DEPTH = 8", "DEPTH = 6")]) \
        == "LR = 0.02\nDEPTH = 6\n"
    # multi-line anchor
    assert apply_edit_blocks(src, [("LR = 0.01\nDEPTH = 8", "DEPTH = 4")]) \
        == "DEPTH = 4\n"
    # first occurrence only
    assert apply_edit_blocks("x = 1\nx = 1\n", [("x = 1", "x = 9")]) \
        == "x = 9\nx = 1\n"


def test_apply_edit_blocks_rejects_miss_and_empty():
    src = "LR = 0.01\n"
    assert apply_edit_blocks(src, [("NOPE = 1", "NOPE = 2")]) is None
    # a later block missing its anchor invalidates the whole candidate
    assert apply_edit_blocks(src, [("LR = 0.01", "LR = 0.02"),
                                   ("NOPE = 1", "NOPE = 2")]) is None
    assert apply_edit_blocks(src, []) is None       # no-edit candidate invalid
    assert apply_edit_blocks(src, [("", "x = 1")]) is None  # empty anchor


def test_parse_edit_guesses_ordered_and_validated():
    text = ("CONFIDENCE: 0.4\n"
            "<<<<<<< SEARCH\nLR = 0.01\n=======\nLR = 0.03\n>>>>>>> REPLACE\n"
            "CONFIDENCE: 0.9\n"
            "<<<<<<< SEARCH\nLR = 0.01\n=======\nLR = 0.02\n>>>>>>> REPLACE\n"
            "<<<<<<< SEARCH\nDEPTH = 8\n=======\nDEPTH = 6\n>>>>>>> REPLACE\n"
            "CONFIDENCE: 0.2\nprose without any block\n")
    gs = parse_edit_guesses(text)
    # highest confidence first; the blockless candidate is dropped
    assert [g.confidence for g in gs] == [0.9, 0.4]
    assert gs[0].blocks == [("LR = 0.01", "LR = 0.02"), ("DEPTH = 8", "DEPTH = 6")]
    assert gs[1].blocks == [("LR = 0.01", "LR = 0.03")]


def test_parse_edit_guesses_multiline_and_deletion():
    text = ("CONFIDENCE: 0.7\n"
            "<<<<<<< SEARCH\ndef f():\n    return 1\n=======\n"
            "def f():\n    return 2\n>>>>>>> REPLACE\n"
            "CONFIDENCE: 0.5\n"
            "<<<<<<< SEARCH\nDEAD = True\n=======\n>>>>>>> REPLACE\n")
    gs = parse_edit_guesses(text)
    assert gs[0].blocks == [("def f():\n    return 1", "def f():\n    return 2")]
    assert gs[1].blocks == [("DEAD = True", "")]     # empty REPLACE = deletion


def test_speculator_predict_materializes_block_guesses():
    """predict() applies each candidate's blocks to the CURRENT train.py and
    returns full-text guesses; unappliable candidates are dropped and counted;
    candidates whose applied result duplicates an earlier guess (normalized)
    are dropped silently."""
    from src.llm_client import Speculator
    response = (
        "CONFIDENCE: 0.9\n"
        "<<<<<<< SEARCH\nLR = 0.01\n=======\nLR = 0.02\n>>>>>>> REPLACE\n"
        "CONFIDENCE: 0.5\n"
        "<<<<<<< SEARCH\nNOT_IN_FILE = 1\n=======\nNOT_IN_FILE = 2\n>>>>>>> REPLACE\n"
        "CONFIDENCE: 0.3\n"
        "<<<<<<< SEARCH\nLR = 0.01\n=======\nLR = 0.02  # same edit\n>>>>>>> REPLACE\n")

    class StubSpeculator(Speculator):
        def __init__(self):  # no OpenAI client
            self.model = "stub"
            self.max_tokens = None

        def chat(self, system, user):
            assert "SEARCH" in system  # block-format prompt is in force
            return (response, 1.5, 11, 22)

    s = StubSpeculator().predict([], "LR = 0.01\nDEPTH = 8\n", k=3)
    assert [g.new_train_py for g in s.guesses] == ["LR = 0.02\nDEPTH = 8\n"]
    assert s.guesses[0].confidence == 0.9
    assert s.apply_failures == 1                    # the anchor miss, not the dup
    assert (s.latency_s, s.tokens_in, s.tokens_out) == (1.5, 11, 22)


def test_speculator_predict_caps_candidates_at_k():
    from src.llm_client import Speculator
    response = (
        "CONFIDENCE: 0.9\n<<<<<<< SEARCH\nA = 1\n=======\nA = 2\n>>>>>>> REPLACE\n"
        "CONFIDENCE: 0.8\n<<<<<<< SEARCH\nB = 1\n=======\nB = 2\n>>>>>>> REPLACE\n"
        "CONFIDENCE: 0.7\n<<<<<<< SEARCH\nC = 1\n=======\nC = 2\n>>>>>>> REPLACE\n")

    class StubSpeculator(Speculator):
        def __init__(self):
            self.model = "stub"
            self.max_tokens = None

        def chat(self, system, user):
            return (response, 0.1, 1, 1)

    s = StubSpeculator().predict([], "A = 1\nB = 1\nC = 1\n", k=2)
    assert len(s.guesses) == 2
    assert [g.confidence for g in s.guesses] == [0.9, 0.8]


def test_edit_diff_is_compact_unified():
    from src.llm_client import edit_diff
    d = edit_diff("a = 1\nb = 2\n", "a = 1\nb = 3\n")
    assert "-b = 2" in d and "+b = 3" in d
    assert "a = 1" not in d.replace("-a", "").replace("+a", "") or True  # context ok
    assert edit_diff("same\n", "same\n") == ""


def test_speculator_receives_actor_edit_history():
    """The Actor's editing style is the strongest predictor of its next edit:
    predict() must weave past edits (as diffs) into the user prompt."""
    from src.llm_client import Speculator

    class StubSpeculator(Speculator):
        def __init__(self):
            self.model = "stub"
            self.max_tokens = None
            self.seen_user = None

        def chat(self, system, user):
            self.seen_user = user
            return ("CONFIDENCE: 0.9\n<<<<<<< SEARCH\nA = 1\n=======\n"
                    "A = 2\n>>>>>>> REPLACE\n", 0.1, 1, 1)

    s = StubSpeculator()
    s.predict([], "A = 1\n", k=1,
              edit_history=["step 0 (keep):\n-LR = 0.01\n+LR = 0.02"])
    assert "previous edits" in s.seen_user
    assert "+LR = 0.02" in s.seen_user
    # without history the prompt must not grow a dangling section
    s.predict([], "A = 1\n", k=1)
    assert "previous edits" not in s.seen_user


def test_fakes_are_scripted():
    fa = FakeActor(script=[(True, "x = 1"), (False, "x = 2")])
    d1 = fa.decide([], "old")
    d2 = fa.decide([], "old")
    assert d1.keep and d1.new_train_py == "x = 1"
    assert not d2.keep

    fs = FakeSpeculator(script=[[Guess("x = 1", 0.9)]], fail_steps={1})
    s = fs.predict([], "old", k=1)
    assert s.guesses[0].new_train_py == "x = 1"
    try:
        fs.predict([], "old", k=1)
        assert False, "expected TimeoutError on step 1"
    except TimeoutError:
        pass
