#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"
FIXTURE="$(pwd)/tests/cas/fixtures/test_policy.lua"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_lua: $BIN is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
if ! grep -q -- "--telemetry" <<<"$HELP" || ! grep -qi "lua" <<<"$HELP"; then
    echo "SKIP test_cas_lua: pending runtime --telemetry=lua support"
    exit 0
fi

if ! command -v lua >/dev/null 2>&1 &&
   ! command -v lua5.4 >/dev/null 2>&1 &&
   ! command -v luajit >/dev/null 2>&1; then
    echo "SKIP test_cas_lua: Lua/LuaJIT runtime is unavailable"
    exit 0
fi

if ! grep -Eq -- "--lua-script|--telemetry-lua-script|lua.*script|script.*lua" <<<"$HELP"; then
    echo "SKIP test_cas_lua: pending Lua script CLI contract"
    exit 0
fi

if [[ ! -f "$FIXTURE" ]]; then
    echo "FAIL test_cas_lua: missing fixture $FIXTURE"
    exit 1
fi

echo "SKIP test_cas_lua: executable Lua daemon integration awaits Task 11 CLI wiring"
