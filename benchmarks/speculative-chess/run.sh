#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

uv run pytest -q

: "${CHESS_ACTOR_BASE_URL:?}"
: "${CHESS_ACTOR_API_KEY:?}"
: "${CHESS_SPEC_BASE_URL:?}"
: "${CHESS_SPEC_API_KEY:?}"

uv run python main.py --mode regular-native --config config.yml
uv run python main.py --mode regular --config config.yml
uv run python main.py --mode spec --config config.yml
echo "per-mode results written under ./results/"
