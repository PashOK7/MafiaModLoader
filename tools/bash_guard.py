"""bash_guard.py: a PreToolUse hook (.claude/settings.json) that stops Bash commands with a doubled backslash.

The agent's Bash tool on this machine turns every doubled backslash in a command into a single one before bash sees it
(measured 2026-10-14 with `od -c`: in a quoted here-document, in single quotes, in an echo alike; only one before a
double quote survived). A file written that way loses its escapes silently (C strings' backslash-n, Windows paths'
doubled backslashes, Python string escapes); AGENTS.md has warned of it since 2026-10-13 and it still happened. So
the command is refused, with what to do instead. Reads the hook's JSON on stdin; prints the decision as JSON.
"""
import json
import sys

DOUBLED = '\\' * 2


def main():
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0   # not a hook's input: nothing to judge
    command = (event.get('tool_input') or {}).get('command') or ''
    if DOUBLED not in command:
        return 0
    reason = ('This command has a doubled backslash, and the Bash tool here turns every doubled backslash into a single '
              'one before bash runs it (in here-documents and single quotes too), so what it writes or passes on would '
              'be silently wrong. Write file contents with the Write or Edit tool (or tools/replace.py for exact '
              'edits); keep Python with escapes in a .py file written by Write; in a command, use single backslashes or '
              'forward slashes in paths.')
    print(json.dumps({'hookSpecificOutput': {'hookEventName': 'PreToolUse', 'permissionDecision': 'deny',
                                             'permissionDecisionReason': reason}}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
