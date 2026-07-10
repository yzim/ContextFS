#!/usr/bin/env bash
# Full comparison: sequential-native, sequential-agentvfs, speculative-agentvfs.
# Requires: build/agentvfs{,-ctl}, uv sync --extra train, prepared autoresearch
# data (~/.cache/autoresearch), ACTOR_/SPEC_ env vars, cgroup access.
set -euo pipefail
cd "$(dirname "$0")"

: "${ACTOR_BASE_URL:?}" "${ACTOR_API_KEY:?}" "${SPEC_BASE_URL:?}" "${SPEC_API_KEY:?}"

uv run pytest -q                       # fast tests must be green
uv run python main.py --mode regular-native --config config.yml
uv run python main.py --mode regular        --config config.yml
uv run python main.py --mode spec           --config config.yml
echo "results in ./results/ — compare totals.csv wall_s across the three runs"
