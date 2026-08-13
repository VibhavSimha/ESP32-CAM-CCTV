#!/usr/bin/env bash
# Host-side unit tests for the captive-portal form auto-detection parser.
# Requires only a C++ compiler (no ESP32 toolchain).
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-c++}"
OUT="$(mktemp -d)/captive_portal_parse_test"

"$CXX" -std=c++11 -Wall -Wextra -I.. \
    test_captive_portal_parse.cpp ../captive_portal_parse.cpp -o "$OUT"

"$OUT"
