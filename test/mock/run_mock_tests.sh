#!/usr/bin/env bash
# Runs test/sql/arcgis_mock.test against test/mock/arcgis_mock_server.py.
# Usage: test/mock/run_mock_tests.sh [path/to/unittest]
set -euo pipefail

cd "$(dirname "$0")/../.."
UNITTEST="${1:-./build/release/test/unittest}"

coproc SERVER { exec python3 test/mock/arcgis_mock_server.py; }
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

read -r ready port <&"${SERVER[0]}"
if [ "$ready" != "READY" ]; then
	echo "mock server failed to start" >&2
	exit 1
fi

ARCGIS_MOCK_URL="http://127.0.0.1:${port}" "$UNITTEST" "test/sql/arcgis_mock.test"
