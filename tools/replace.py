"""replace.py <spec>: exact text replacements in files, with no escaping anywhere.

Edits written as Python (or shell) string literals break on backslashes: \\n, \\b, \\f, \\N in C++ strings and
Windows paths turn into other characters or syntax errors. A spec file holds the old and the new text verbatim:

    === FILE src/water.cpp
    --- OLD
    text exactly as it is in the file
    --- NEW
    the text to put there
    === END

Several FILE blocks may follow each other, and one file may appear several times. Paths are relative to the
repository root (this script's parent folder) unless absolute. Every OLD text must occur exactly once in its file;
if any does not, nothing is written. Files keep their line endings (LF or CRLF) and their encoding (UTF-8, with or
without BOM). Marker lines are matched exactly at the start of a line.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse(spec_text):
    edits = []   # (path, old, new)
    lines = spec_text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i].rstrip('\r')
        if not line.startswith('=== FILE '):
            if line.strip():
                sys.exit(f'spec line {i + 1}: expected "=== FILE <path>", got {line!r}')
            i += 1
            continue
        path = line[len('=== FILE '):].strip()
        i += 1
        if i >= len(lines) or lines[i].rstrip('\r') != '--- OLD':
            sys.exit(f'spec line {i + 1}: expected "--- OLD" after FILE {path}')
        i += 1
        old = []
        while i < len(lines) and lines[i].rstrip('\r') != '--- NEW':
            old.append(lines[i].rstrip('\r'))
            i += 1
        if i >= len(lines):
            sys.exit(f'spec: FILE {path}: no "--- NEW"')
        i += 1
        new = []
        while i < len(lines) and lines[i].rstrip('\r') != '=== END':
            new.append(lines[i].rstrip('\r'))
            i += 1
        if i >= len(lines):
            sys.exit(f'spec: FILE {path}: no "=== END"')
        i += 1
        edits.append((path, '\n'.join(old), '\n'.join(new)))
    return edits


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding='utf-8-sig') as f:
        edits = parse(f.read())
    if not edits:
        sys.exit('spec: no edits')
    files = {}   # path -> [text with LF, bom, crlf]
    for path, old, new in edits:
        full = path if os.path.isabs(path) else os.path.join(ROOT, path)
        if full not in files:
            raw = open(full, 'rb').read()
            bom = raw.startswith(b'\xef\xbb\xbf')
            text = raw[3:].decode('utf-8') if bom else raw.decode('utf-8')
            crlf = '\r\n' in text
            files[full] = [text.replace('\r\n', '\n'), bom, crlf]
        text = files[full][0]
        n = text.count(old)
        if n != 1:
            sys.exit(f'{path}: the OLD text occurs {n} times (must be once); nothing written:\n{old[:300]}')
        files[full][0] = text.replace(old, new, 1)
    for full, (text, bom, crlf) in files.items():
        if crlf:
            text = text.replace('\n', '\r\n')
        data = text.encode('utf-8')
        with open(full, 'wb') as f:
            f.write((b'\xef\xbb\xbf' if bom else b'') + data)
    print(f'{len(edits)} replacement(s) in {len(files)} file(s)')


if __name__ == '__main__':
    main()
