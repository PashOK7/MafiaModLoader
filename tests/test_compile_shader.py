"""Small build-tool regressions; no game, GPU, full build or long wait required."""
import argparse
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('compile_shader', Path(__file__).resolve().parents[1] / 'tools/compile_shader.py')
shader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shader)


class ShaderBuildTests(unittest.TestCase):
    def test_dxc_language_version_reaches_both_phases(self):
        self.args.kind = 'dxc'
        self.args.hlsl_version = '2018'
        commands = []
        def phase(command, name, timeout, log):
            commands.append(command)
            marker = '-Fi' if name == 'preprocess' else '-Fh'
            Path(command[command.index(marker) + 1]).write_text('new header', encoding='utf-8')
            return .01
        with patch.object(shader, 'run_phase', side_effect=phase):
            shader.compile_shader(self.args)
        self.assertEqual(len(commands), 2)
        for command in commands:
            self.assertEqual(command[command.index('-HV') + 1], '2018')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source.hlsl'
        self.source.write_text('float4 Main():SV_Target{return 1;}', encoding='utf-8')
        self.output = self.root / 'Main.h'
        self.output.write_text('accepted header', encoding='utf-8')
        self.args = argparse.Namespace(source=self.source, output=self.output, compiler=Path(sys.executable),
            include=[], kind='fxc', entry='Main', name='Main', profile='ps_5_0',
            depfile=self.root / 'Main.h.d', timeout=.15, preprocess_timeout=1)

    def phase(self, command, phase, timeout, log):
        if phase == 'preprocess':
            Path(command[command.index('/P') + 1]).write_text('#line 1 "' + self.source.as_posix() + '"\n', encoding='utf-8')
        else:
            Path(command[command.index('/Fh') + 1]).write_text('new header', encoding='utf-8')
        return .01

    def test_timeout_terminates_child(self):
        started = time.monotonic()
        with tempfile.TemporaryFile(mode='w+', encoding='utf-8') as log:
            with self.assertRaisesRegex(shader.BuildError, 'timed out'):
                shader.run_phase([sys.executable, '-c', 'import time; time.sleep(10)'], 'compile O3', .15, log)
        self.assertLess(time.monotonic() - started, 3)

    def test_lock_refuses_duplicate(self):
        with shader.output_lock(self.root / 'output.lock'):
            with self.assertRaisesRegex(shader.BuildError, 'another build owns'):
                with shader.output_lock(self.root / 'output.lock'):
                    self.fail('second build entered')
        with shader.output_lock(self.root / 'output.lock'):
            pass

    def test_timeout_keeps_header_and_identical_retry_is_immediate(self):
        def timeout(command, phase, seconds, log):
            if phase == 'preprocess':
                return self.phase(command, phase, seconds, log)
            raise shader.BuildError('compile O3 timed out after .15s; compiler terminated')
        with patch.object(shader, 'run_phase', side_effect=timeout) as run:
            with self.assertRaisesRegex(shader.BuildError, 'timed out'):
                shader.compile_shader(self.args)
            self.assertEqual(self.output.read_text(), 'accepted header')
            run.reset_mock()
            with self.assertRaisesRegex(shader.BuildError, 'identical inputs'):
                shader.compile_shader(self.args)
            self.assertEqual(run.call_count, 1)  # preprocessing only; no optimizer
            self.source.write_text('changed source', encoding='utf-8')
            run.side_effect = self.phase
            shader.compile_shader(self.args)
        self.assertEqual(self.output.read_text(), 'new header')
        self.assertFalse(Path(str(self.output) + '.timeout.json').exists())

    def test_changed_inputs_discard_output(self):
        def mutation(command, phase, timeout, log):
            result = self.phase(command, phase, timeout, log)
            if phase != 'preprocess':
                self.source.write_text('concurrent edit', encoding='utf-8')
            return result
        with patch.object(shader, 'run_phase', side_effect=mutation):
            with self.assertRaisesRegex(shader.BuildError, 'inputs changed'):
                shader.compile_shader(self.args)
        self.assertEqual(self.output.read_text(), 'accepted header')

    def test_actual_dependencies_and_escaping(self):
        included = self.root / 'included file.hlsli'
        included.write_text('// included', encoding='utf-8')
        text = '#line 1 "' + str(included).replace('\\', '\\\\') + '"\n#line 8\n'
        self.assertEqual(set(shader.dependencies(text, self.source)), {self.source, included})
        self.assertIn('included\\ file.hlsli', shader.dep_escape(included))


if __name__ == '__main__':
    unittest.main()
