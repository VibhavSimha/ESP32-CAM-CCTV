#!/usr/bin/env bash
# Host-side unit tests for the captive-portal form auto-detection parser.
# Requires only a C++ compiler (no ESP32 toolchain).
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-c++}"
OUT_DIR="$(mktemp -d)"
CPT_OUT="$OUT_DIR/captive_portal_parse_test"
RES_OUT="$OUT_DIR/resilience_policy_test"

"$CXX" -std=c++11 -Wall -Wextra -I.. \
    test_captive_portal_parse.cpp ../captive_portal_parse.cpp -o "$CPT_OUT"

"$CPT_OUT"

"$CXX" -std=c++11 -Wall -Wextra -I.. \
    test_resilience_policy.cpp ../resilience_policy.cpp -o "$RES_OUT"

"$RES_OUT"
