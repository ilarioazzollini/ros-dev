#!/usr/bin/env bash
# clangd wants one compilation database for the whole workspace; colcon writes
# one per package under build/<pkg>/. Merge them into build/ so editing any
# file works from a single clangd instance (see
# .devcontainer/devcontainer.json's clangd.arguments,
# --compile-commands-dir=.../ros2_ws/build).
#
# Sourced (not executed) by build.sh/clean_build.sh right after colcon build,
# from ros2_ws/ -- relies on their cwd, not its own location.
set -e

python3 - <<'PY'
import json
import pathlib

entries = []
for db in sorted(pathlib.Path("build").glob("*/compile_commands.json")):
    entries.extend(json.loads(db.read_text()))

pathlib.Path("build/compile_commands.json").write_text(json.dumps(entries, indent=2))
PY
