#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"
FIXTURE="$(pwd)/tests/cas/fixtures/test_filter.wat"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_wasm: $BIN is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
if ! grep -q -- "--telemetry" <<<"$HELP" || ! grep -qi "wasm" <<<"$HELP"; then
    echo "SKIP test_cas_wasm: pending runtime --telemetry=wasm support"
    exit 0
fi

if ! command -v wat2wasm >/dev/null 2>&1; then
    echo "SKIP test_cas_wasm: wat2wasm is unavailable"
    exit 0
fi

if ! command -v iwasm >/dev/null 2>&1; then
    echo "SKIP test_cas_wasm: WAMR runtime command iwasm is unavailable"
    exit 0
fi

if ! grep -Eq -- "--wasm-module|--telemetry-wasm-module|wasm.*module|module.*wasm" <<<"$HELP"; then
    echo "SKIP test_cas_wasm: pending Wasm module CLI contract"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-wasm}"
MODULE="$ROOT/test_filter.wasm"

cleanup() {
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$ROOT"
wat2wasm "$FIXTURE" -o "$MODULE"

echo "SKIP test_cas_wasm: executable Wasm daemon integration awaits Task 11 CLI wiring"
