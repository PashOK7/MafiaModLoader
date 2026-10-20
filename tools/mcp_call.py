"""mcp_call.py: one tool of the freshly built MCP server, called from a terminal.

    python tools/mcp_call.py <tool> [<json arguments> | @file.json]
    python tools/mcp_call.py bench '{"game": "1.2", "frames": 200}'

An agent's session keeps the MCP server it started with (its binary locked: AGENTS.md); a server just built is tried
so before the session's next start: this starts mcp/target/release/mml-mcp.exe, calls the tool and prints its text
(exit 1 when the tool answered an error). The games are mml.config.json's, as the session's server sees them.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCP = os.path.join(ROOT, 'mcp', 'target', 'release', 'mml-mcp.exe')


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0
    name = sys.argv[1]
    args = {}
    if len(sys.argv) > 2:
        a = sys.argv[2]
        if a.startswith('@'):
            with open(a[1:], encoding='utf-8') as f:
                a = f.read()
        args = json.loads(a)
    p = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         env=dict(os.environ, MML_REPO=ROOT))
    n = 0

    def request(method, params):
        nonlocal n
        n += 1
        p.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': n, 'method': method, 'params': params}) + '\n').encode())
        p.stdin.flush()
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError('the server closed: ' + p.stderr.read().decode(errors='replace'))
            msg = json.loads(line)
            if msg.get('id') == n:
                if 'error' in msg:
                    raise RuntimeError(msg['error'])
                return msg['result']

    request('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'mcp_call', 'version': '1'}})
    p.stdin.write((json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n').encode())
    p.stdin.flush()
    r = request('tools/call', {'name': name, 'arguments': args})
    sys.stdout.buffer.write(''.join(c.get('text', '') for c in r.get('content', [])).encode('utf-8') + b'\n')
    p.stdin.close()
    p.wait(timeout=10)
    return 1 if r.get('isError') else 0


if __name__ == '__main__':
    sys.exit(main())
