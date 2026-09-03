#!/usr/bin/env bash
# PostToolUse: clang-format any C++ file Claude just wrote or edited.
# Silent no-op when clang-format is not installed or the file isn't C++.
f="$(python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("file_path") or d.get("tool_response",{}).get("filePath") or "")')"
[ -n "$f" ] || exit 0
case "$f" in *.cpp|*.cc|*.h|*.hpp) ;; *) exit 0 ;; esac
command -v clang-format >/dev/null || exit 0
[ -f "$f" ] && clang-format -i --style=file "$f"
exit 0
