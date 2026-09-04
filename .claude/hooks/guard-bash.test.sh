#!/usr/bin/env bash
# Regression tests for guard-bash.py. Run: bash .claude/hooks/guard-bash.test.sh
# Cases live in a heredoc so this script's own text never trips the guard.
cd "$(dirname "$0")"
pass=0; fail=0
check() {  # $1 = expected (DENY|allow), rest = command text
  local want="$1"; shift
  local out; out="$(python3 -c 'import json,sys;print(json.dumps({"tool_name":"Bash","tool_input":{"command":sys.argv[1]}}))' "$*" | python3 guard-bash.py)"
  local got; [ -n "$out" ] && got=DENY || got=allow
  if [ "$got" = "$want" ]; then pass=$((pass+1)); printf "  ok    %-5s %s\n" "$got" "$*"
  else fail=$((fail+1)); printf "  FAIL  want %s got %s: %s\n" "$want" "$got" "$*"; fi
}
while IFS='|' read -r want cmd; do [ -n "$want" ] && check "$want" "$cmd"; done <<'CASES'
DENY|gh repo create foo
DENY|cd x && gh pr list
DENY|git commit --no-verify -m x
DENY|git remote add origin git@github.com:janu-droid/x.git
DENY|git clone https://github.com/x/y.git
DENY|git config user.email hello@chardi.ai
allow|git config --local user.email januchaudhary2004@gmail.com
allow|git remote add origin github-januchaudhary:Janu-Chaudhary/opentune.git
allow|cmake --build build -j
allow|echo the gh tool is banned here
allow|grep -rn no-verify constitution.md
DENY|cat take.wav
DENY|xxd foo.wav
DENY|strings x.mp3
DENY|head -c 100 a.flac
DENY|od -c b.aiff
DENY|tail -c 200 clip.m4a
DENY|less song.ogg
DENY|more song.aac
DENY|hexdump -C clip.mp4
DENY|base64 take.wav
DENY|echo start && cat take.wav
DENY|x=$(< take.wav)
allow|ls -l take.wav
allow|stat take.wav
allow|./build/tools/f0-dump/opentune-f0dump take.wav yin 256
allow|.venv/bin/python tools/analyze.py take.wav --out r.png
allow|cp a.wav b.wav
allow|mv a.wav b.wav
allow|find . -name '*.wav'
allow|wc -c take.wav
allow|file take.wav
allow|gst-launch-1.0 filesink location=out.wav
allow|echo never cat a .wav into context
CASES
# heredoc bodies must be invisible to the guard
hd=$'cat > x.md <<\'EOF\'\nnever use --no-verify or gh here\nEOF'
check allow "$hd"
hd2=$'cat > x.md <<\'EOF\'\ntext\nEOF\ngit commit --no-verify -m x'
check DENY "$hd2"
hd3=$'cat > report.md <<\'EOF\'\nDENY commands like `cat take.wav` or `xxd foo.wav` dump audio bytes.\nEOF'
check allow "$hd3"
echo "passed=$pass failed=$fail"; [ "$fail" -eq 0 ]
