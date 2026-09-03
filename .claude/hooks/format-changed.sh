#!/usr/bin/env bash
# PostToolUse on Bash: format every modified or new C++ file in the working tree.
# Closes the gap where files written by shell commands bypass the Edit/Write hook.
command -v clang-format >/dev/null || exit 0
cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0
git ls-files -mo --exclude-standard -- '*.cpp' '*.cc' '*.h' '*.hpp' 2>/dev/null \
  | grep -v '^third_party/' | grep -v '^build/' \
  | while read -r f; do [ -f "$f" ] && clang-format -i --style=file "$f"; done
exit 0
