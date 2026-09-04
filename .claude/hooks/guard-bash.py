#!/usr/bin/env python3
"""PreToolUse guard for Bash. See constitution.md IX and CLAUDE.md "Audio and verification".

Denies commands that would act as the wrong GitHub identity, bypass the
repository's git hooks, or dump raw audio bytes into the agent's context.
Reads the hook JSON on stdin, writes a decision on stdout.
"""
import json, re, sys

try:
    cmd = json.load(sys.stdin).get("tool_input", {}).get("command", "")
except Exception:
    sys.exit(0)  # malformed input: don't block, don't crash


def strip_heredocs(text):
    """Drop heredoc bodies so rules see commands, not document content.

    Writing the sentence "never use --no-verify" into a markdown file is not using
    --no-verify. Without this, the guard blocks editing the docs that describe it.
    """
    out, lines, i = [], text.split("\n"), 0
    while i < len(lines):
        line = lines[i]
        m = re.search(r"<<-?\s*['\"]?(\w+)['\"]?", line)
        out.append(line)
        i += 1
        if m:
            term = m.group(1)
            while i < len(lines) and lines[i].strip() != term:
                i += 1
            i += 1  # skip the terminator line itself
    return "\n".join(out)


cmd = strip_heredocs(cmd)

# Patterns are anchored to *command position* (start of line or after ; & | ( $( )
# so a word inside an echo string or a comment does not count as invoking it.
RULES = [
    (r"(?m)(^|[;&|(]|\$\()\s*gh(\s|$)",
     "`gh` is authenticated as janu-droid, the wrong account. Constitution IX."),
    (r"\bgit\b[^\n;&|]*--no-verify\b",
     "--no-verify bypasses the identity hooks. Constitution IX."),
    (r"\bgit\s+(remote|clone|push|pull|fetch)\b.*(git@github\.com|https://github\.com|github\.com:)",
     "Plain github.com authenticates as janu-droid. Use the github-januchaudhary: alias."),
    (r"\bgit\s+config\b.*user\.(email|name)\b(?!.*(januchaudhary2004@gmail\.com|Janu-Chaudhary))",
     "Only Janu-Chaudhary <januchaudhary2004@gmail.com> may be set. Constitution IX."),
    (r"(?m)(^|[;&|(]|\$\()\s*(cat|head|tail|less|more|strings|xxd|od|hexdump|base64)\b"
     r"[^;&|\n]*\.(wav|flac|mp3|aac|ogg|m4a|aiff?|mp4)\b(?![\w.])",
     "Audio files must not be dumped into context (binary poisons it). "
     "Use .venv/bin/python tools/analyze.py <file> for pitch/cents/spectrogram instead. "
     "CLAUDE.md 'Audio and verification'."),
    # No command-position anchor here: `$(...)` and `` `...` `` execute wherever they
    # appear (even inside double quotes), so this doesn't need to be an anchored rule
    # the way plain command names do.
    (r"\$\(\s*<\s*[^)\n]*\.(wav|flac|mp3|aac|ogg|m4a|aiff?|mp4)\b[^)\n]*\)",
     "`$(< file)` reads the whole audio file into context, same as cat. "
     "Use .venv/bin/python tools/analyze.py <file> instead. CLAUDE.md 'Audio and verification'."),
]

for pattern, reason in RULES:
    if re.search(pattern, cmd):
        print(json.dumps({
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": "BLOCKED by OpenTune guard: " + reason,
            }
        }))
        sys.exit(0)
sys.exit(0)
