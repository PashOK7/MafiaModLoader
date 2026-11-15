"""Bounded FXC/DXC invocation for CMake. Never publish an incomplete/stale shader.

Preprocessing discovers the compiler's actual active includes and reports macro/include
errors before optimization. A timed-out input is refused on the next identical attempt.
The lock is non-waiting and released by the OS even when the build is interrupted.
"""
import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


class BuildError(Exception):
    pass


@contextlib.contextmanager
def output_lock(path):
    with path.open('a+b') as handle:
        if os.name == 'nt':
            import msvcrt
            try:
                if os.fstat(handle.fileno()).st_size == 0:
                    handle.write(b'0')
                    handle.flush()
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise BuildError(f'another build owns {path}; no second compiler started') from error
        else:
            import fcntl
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise BuildError(f'another build owns {path}; no second compiler started') from error
        try:
            yield
        finally:
            if os.name == 'nt':
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def dependencies(text, source):
    paths = {source.resolve()}
    for match in re.finditer(r'^\s*#(?:line\s+)?\s*\d+\s+"([^"\r\n]+)"', text, re.MULTILINE):
        spelling = match.group(1)
        if spelling.startswith('<'):
            continue
        # FXC escapes Windows backslashes in #line filenames; DXC may use slashes.
        path = Path(spelling.replace('\\\\', '\\')).resolve()
        if not path.is_file():
            raise BuildError(f'preprocessor dependency is missing: {path}')
        paths.add(path)
    return sorted(paths, key=str)


def fingerprint(paths, options):
    digest = hashlib.sha256(json.dumps(options, sort_keys=True).encode())
    for path in paths:
        digest.update(str(path).encode('utf-8'))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def dep_escape(path):
    return path.as_posix().replace('$', '$$').replace('#', '\\#').replace(' ', '\\ ').replace(':', '\\:')


def run_phase(command, phase, timeout, log):
    print(f'shader: {phase} (limit {timeout:g}s)', flush=True)
    log.write(f'\n[{phase}] {subprocess.list2cmdline(command)}\n')
    log.flush()
    started = time.monotonic()
    # FXC/DXC are direct child processes (no shell/launcher). subprocess.run kills
    # and reaps that exact compiler on timeout; unrelated builds are never killed.
    try:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise BuildError(f'{phase} timed out after {timeout:g}s; compiler terminated') from error
    elapsed = time.monotonic() - started
    log.write(f'[{phase}] exit={result.returncode}, seconds={elapsed:.3f}\n')
    log.flush()
    if result.returncode:
        raise BuildError(f'{phase} failed (exit {result.returncode})')
    return elapsed


def compile_shader(args):
    source, output = args.source.resolve(), args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    log_path = Path(str(output) + '.log')
    timeout_path = Path(str(output) + '.timeout.json')
    with output_lock(Path(str(output) + '.lock')):
        with log_path.open('w', encoding='utf-8') as log, tempfile.TemporaryDirectory(prefix=output.stem + '-', dir=output.parent) as scratch:
            scratch = Path(scratch)
            preprocessed, pending = scratch / 'preprocessed.hlsl', scratch / output.name
            prefix = '-' if args.kind == 'dxc' else '/'
            common = [str(args.compiler), prefix + 'nologo']
            language_version = getattr(args, 'hlsl_version', None)
            if language_version:
                if args.kind != 'dxc':
                    raise BuildError('an explicit HLSL version requires dxc')
                common += ['-HV', language_version]
            for include in args.include:
                common += [prefix + 'I', str(include.resolve())]
            target = [prefix + 'T', args.profile, prefix + 'E', args.entry]
            # FXC refuses /T or /E together with /P ("cannot preprocess ... and
            # compile at the same time"). DXC accepts the target context with -P.
            preprocess = common + [prefix + 'P', str(preprocessed), str(source)]
            if args.kind == 'dxc':
                preprocess = common + target + ['-P', '-Fi', str(preprocessed), str(source)]
            print(f'shader: {source.name}:{args.entry} ({args.profile}), log={log_path}', flush=True)
            signature = None
            try:
                run_phase(preprocess, 'preprocess', args.preprocess_timeout, log)
                paths = dependencies(preprocessed.read_text(encoding='utf-8-sig'), source)
                compiler_stat = args.compiler.stat()
                options = dict(compiler=str(args.compiler.resolve()), compiler_stat=[compiler_stat.st_size, compiler_stat.st_mtime_ns],
                               kind=args.kind, entry=args.entry, profile=args.profile, name=args.name,
                               includes=[str(p.resolve()) for p in args.include], optimization='O3', timeout=args.timeout)
                if language_version:
                    options['hlsl_version'] = language_version
                signature = fingerprint(paths, options)
                if timeout_path.exists():
                    previous = json.loads(timeout_path.read_text(encoding='utf-8'))
                    if previous.get('fingerprint') == signature:
                        raise BuildError(f'identical inputs already timed out; fix the shader before retrying (record: {timeout_path})')
                command = common + [prefix + 'O3', prefix + 'WX', prefix + 'T', args.profile,
                                    prefix + 'E', args.entry, prefix + 'Vn', 'k' + args.name,
                                    prefix + 'Fh', str(pending), str(source)]
                elapsed = run_phase(command, 'compile O3', args.timeout, log)
                if not pending.is_file() or pending.stat().st_size == 0:
                    raise BuildError('compiler succeeded without a nonempty output')
                if fingerprint(paths, options) != signature:
                    raise BuildError('shader inputs changed during compilation; output discarded')
                # The old good header survives all failures. A failed command still blocks
                # its consuming C++ target; old bytecode is never reported as a new build.
                args.depfile.write_text(dep_escape(output) + ': ' + ' '.join(dep_escape(p) for p in paths) + '\n', encoding='utf-8')
                os.replace(pending, output)
                timeout_path.unlink(missing_ok=True)
                print(f'shader: {args.entry} OK in {elapsed:.2f}s; {len(paths)} active source files', flush=True)
            except BuildError as error:
                if signature and 'compile O3 timed out' in str(error):
                    timeout_path.write_text(json.dumps({'fingerprint': signature, 'error': str(error), 'entry': args.entry}, indent=2), encoding='utf-8')
                log.write(f'ERROR: {error}\n')
                log.flush()
                print(log_path.read_text(encoding='utf-8', errors='replace'), file=sys.stderr)
                raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=['fxc', 'dxc'], required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--entry', required=True)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--hlsl-version', choices=['2016', '2017', '2018', '2021'])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--depfile', type=Path, required=True)
    parser.add_argument('--include', type=Path, action='append', default=[])
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--preprocess-timeout', type=float, default=10)
    args = parser.parse_args()
    if any(not math.isfinite(x) or x <= 0 for x in (args.timeout, args.preprocess_timeout)):
        parser.error('timeouts must be finite and positive; unbounded compilation is forbidden')
    try:
        compile_shader(args)
    except (BuildError, OSError, ValueError) as error:
        print(f'shader ERROR: {args.entry} ({args.profile}): {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
