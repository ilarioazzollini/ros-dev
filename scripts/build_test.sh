#!/usr/bin/env bash
# Fast incremental build + test of the workspace for day-to-day development.
# Runs ./build.sh (incremental, only rebuilds what changed) and then runs the
# package test suites with colcon. Use ./clean_build_test.sh instead the first
# time, after changing dependencies, or whenever the workspace is in a bad state.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build first. Sourcing (not executing) keeps the workspace overlay this script
# sourced via install/setup.bash on $PATH so colcon test sees the fresh build,
# and reuses the same COLCON_PARALLEL_WORKERS / MAKEFLAGS RAM caps.
source "${SCRIPT_DIR}/build.sh"

# Run the test suites. With no --packages-select, colcon tests every package it
# discovers under ros2_ws/src/. console_direct+ streams test output live so
# failures are visible as they happen.
colcon test \
    --parallel-workers "${COLCON_PARALLEL_WORKERS}" \
    --event-handlers console_direct+

# Aggregate the results. test-result --verbose prints every failing test and
# exits non-zero if any test failed, so (with set -e) this script fails loudly
# when the suite is not green -- the gate we want before proposing a PR.
colcon test-result --verbose
