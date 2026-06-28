#!/usr/bin/env bash
# Full, from-scratch build + test of the workspace.
# Runs ./clean_build.sh (wipes artifacts, refreshes dependencies, rebuilds
# everything) and then runs the package test suites with colcon. Use this the
# first time, after changing dependencies, or whenever the workspace is in a bad
# state. For day-to-day work prefer ./build_test.sh.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Clean-build first. Sourcing (not executing) keeps the workspace overlay on
# $PATH so colcon test sees the fresh build, and reuses the same
# COLCON_PARALLEL_WORKERS / MAKEFLAGS RAM caps.
source "${SCRIPT_DIR}/clean_build.sh"

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
