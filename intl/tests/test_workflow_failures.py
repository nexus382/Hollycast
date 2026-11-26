import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
INTL_ROOT = REPOSITORY_ROOT / 'intl'
SYNTHETIC_CREDENTIAL = 'synthetic-workflow-secret-011'


class WorkflowFailureTest(unittest.TestCase):
   def run_wrapper(self, wrapper_name, statuses=None):
      statuses = statuses or {}
      wrapper_path = INTL_ROOT / wrapper_name
      with tempfile.TemporaryDirectory() as temporary_directory:
         temp_root = Path(temporary_directory)
         bin_path = temp_root / 'bin'
         bin_path.mkdir()
         log_path = temp_root / 'stages.log'
         fake_python = bin_path / 'python3'
         fake_python.write_text(
            f'''#!{sys.executable}
import json
import os
import pathlib
import sys

script_name = pathlib.Path(sys.argv[1]).stem
stage_names = {{
    'crowdin_prep': 'preparation',
    'crowdin_source_upload': 'source upload',
    'crowdin_translation_download': 'translation download',
    'crowdin_translate': 'translation generation',
}}
stage = stage_names[script_name]
with open(os.environ['WORKFLOW_STAGE_LOG'], 'a', encoding='utf-8') as log_file:
    log_file.write(stage + '\\n')
statuses = json.loads(os.environ['WORKFLOW_STAGE_STATUSES'])
raise SystemExit(statuses.get(stage, 0))
''',
            encoding='utf-8',
         )
         fake_python.chmod(fake_python.stat().st_mode | stat.S_IXUSR)
         environment = os.environ.copy()
         environment['PATH'] = str(bin_path) + os.pathsep + environment.get('PATH', '')
         environment['WORKFLOW_STAGE_LOG'] = str(log_path)
         environment['WORKFLOW_STAGE_STATUSES'] = json.dumps(statuses)

         result = subprocess.run(
            [sys.executable, str(wrapper_path), SYNTHETIC_CREDENTIAL, 'test-core', '/tmp/test-core.h'],
            cwd=REPOSITORY_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
         )
         stages = log_path.read_text(encoding='utf-8').splitlines() if log_path.exists() else []
         return result, stages

   def test_failed_prep_stops_pipeline(self):
      result, stages = self.run_wrapper('upload_workflow.py', {'preparation': 7})

      self.assertEqual(result.returncode, 7)
      self.assertEqual(stages, ['preparation'])
      self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)
      self.assertIn('preparation failed (exit 7)', result.stderr)

   def test_upload_workflow_stops_at_each_failed_stage(self):
      expected = ['preparation', 'source upload']
      for failing_stage in expected:
         with self.subTest(stage=failing_stage):
            result, stages = self.run_wrapper('upload_workflow.py', {failing_stage: 9})
            self.assertEqual(result.returncode, 9)
            self.assertEqual(stages, expected[:expected.index(failing_stage) + 1])
            self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)

   def test_download_workflow_stops_at_each_failed_stage(self):
      expected = ['preparation', 'translation download', 'translation generation']
      for failing_stage in expected:
         with self.subTest(stage=failing_stage):
            result, stages = self.run_wrapper('download_workflow.py', {failing_stage: 11})
            self.assertEqual(result.returncode, 11)
            self.assertEqual(stages, expected[:expected.index(failing_stage) + 1])
            self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)

   def test_successful_workflows_keep_stage_order(self):
      cases = (
         ('upload_workflow.py', ['preparation', 'source upload']),
         ('download_workflow.py', ['preparation', 'translation download', 'translation generation']),
      )
      for wrapper_name, expected in cases:
         with self.subTest(wrapper=wrapper_name):
            result, stages = self.run_wrapper(wrapper_name)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(stages, expected)
            self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)

   def make_crowdin_script_fixture(self, temp_root, script_name):
      isolated_intl = temp_root / 'intl'
      isolated_intl.mkdir()
      for filename in ('crowdin_source_upload.py', 'crowdin_translation_download.py', 'crowdin_cli.py',
                       'core_option_translation.py', 'core_option_regex.py'):
         shutil.copyfile(INTL_ROOT / filename, isolated_intl / filename)

      yaml_path = isolated_intl / 'crowdin.yaml'
      original_config = (
         b'# nonstandard spacing and CRLF are intentionally preserved\r\n'
         b'"project_id": "test-project"\r\n'
         b'"api_token_env": "UNUSED_FIXTURE_VARIABLE"\r\n'
         b'"api_token": "_secret_"\r\n'
         b'"base_url": "https://api.crowdin.com"\r\n'
         b'"files": [ { "source": "/_core_name_/_us.json" } ]\r\n'
      )
      yaml_path.write_bytes(original_config)
      (isolated_intl / 'crowdin-cli.jar').write_bytes(b'local test placeholder')
      return isolated_intl / script_name, yaml_path, original_config

   def run_crowdin_helper(self, script_name, java_status=None, java_available=True, overlap=False):
      with tempfile.TemporaryDirectory() as temporary_directory:
         temp_root = Path(temporary_directory)
         script_path, yaml_path, original_config = self.make_crowdin_script_fixture(temp_root, script_name)
         bin_path = temp_root / 'bin'
         bin_path.mkdir()
         java_log = temp_root / 'java.log'

         if java_available:
            fake_java = bin_path / 'java'
            fake_java.write_text(
               f'''#!{sys.executable}
import os
import pathlib
import subprocess
import sys

args = sys.argv[1:]
config_path = pathlib.Path(args[args.index('--config') + 1])
config = config_path.read_bytes()
directory = pathlib.Path(__file__).resolve().parent.parent / 'intl'
credential = os.environ['WORKFLOW_EXPECTED_CREDENTIAL']
core = os.environ['WORKFLOW_EXPECTED_CORE']
assert config_path.parent == directory and config_path.name != 'crowdin.yaml'
assert b'"api_token_env": "CROWDIN_PERSONAL_TOKEN"' in config
assert config.count(b'api_token_env') == 1 and b'"api_token":' not in config
assert os.environ['CROWDIN_PERSONAL_TOKEN'] == credential and credential.encode() not in config
assert b'/_core_name_/' not in config and ('/' + core + '/_us.json').encode() in config
assert b'"api_token": "_secret_"' in (directory / 'crowdin.yaml').read_bytes()

if os.environ.get('WORKFLOW_OVERLAP') == '1':
    nested_env = dict(os.environ, WORKFLOW_OVERLAP='0', WORKFLOW_EXPECTED_CORE='other-core',
                      WORKFLOW_EXPECTED_CREDENTIAL='nested-test-value',
                      WORKFLOW_JAVA_LOG=os.environ['WORKFLOW_JAVA_LOG'] + '.nested')
    subprocess.run([sys.executable, str(directory / 'crowdin_translation_download.py'),
                    'nested-test-value', 'other-core'], env=nested_env, check=True)
    assert config_path.read_bytes() == config
with open(os.environ['WORKFLOW_JAVA_LOG'], 'w', encoding='utf-8') as log_file:
    log_file.write(args[2])
raise SystemExit(int(os.environ['WORKFLOW_JAVA_STATUS']))
''',
               encoding='utf-8',
            )
            fake_java.chmod(fake_java.stat().st_mode | stat.S_IXUSR)

         environment = os.environ.copy()
         environment['PATH'] = str(bin_path)
         environment['WORKFLOW_JAVA_LOG'] = str(java_log)
         environment['WORKFLOW_JAVA_STATUS'] = str(java_status or 0)
         environment['WORKFLOW_EXPECTED_CREDENTIAL'] = SYNTHETIC_CREDENTIAL
         environment['WORKFLOW_EXPECTED_CORE'] = 'test-core'
         environment['WORKFLOW_OVERLAP'] = '1' if overlap else '0'
         result = subprocess.run(
            [sys.executable, str(script_path), SYNTHETIC_CREDENTIAL, 'test-core'],
            cwd=script_path.parent,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
         )
         log = java_log.read_text(encoding='utf-8') if java_log.exists() else None
         config_after = yaml_path.read_bytes()
         self.assertEqual(list(yaml_path.parent.glob('.crowdin-*.yaml')), [])
         if overlap:
            self.assertEqual((bin_path.parent / 'java.log.nested').read_text(), 'download')
         return result, original_config, config_after, log

   def test_overlapping_upload_and_download_keep_separate_configs(self):
      result, original, after, operation = self.run_crowdin_helper('crowdin_source_upload.py', overlap=True)
      self.assertEqual(result.returncode, 0, result.stderr)
      self.assertEqual(operation, 'upload')
      self.assertEqual(after, original)
      self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)
      self.assertNotIn('nested-test-value', result.stdout + result.stderr)

   def test_java_failure_and_start_error_restore_config_and_redact(self):
      cases = (
         ('crowdin_source_upload.py', 'upload', 23, True),
         ('crowdin_source_upload.py', None, 1, False),
         ('crowdin_translation_download.py', 'download', 23, True),
         ('crowdin_translation_download.py', None, 1, False),
      )
      for script_name, expected_command, expected_status, java_available in cases:
         with self.subTest(script=script_name, command=expected_command):
            result, original, restored, java_command = self.run_crowdin_helper(
               script_name,
               java_status=23 if expected_command else None,
               java_available=java_available,
            )
            self.assertEqual(result.returncode, expected_status)
            self.assertEqual(restored, original)
            self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)
            if expected_command:
               self.assertEqual(java_command, expected_command)
               self.assertIn('failed (exit 23)', result.stderr)
            else:
               self.assertIsNone(java_command)
               self.assertIn('failed', result.stderr)

   def test_java_success_restores_config_and_returns_zero(self):
      for script_name, expected_command in (
         ('crowdin_source_upload.py', 'upload'),
         ('crowdin_translation_download.py', 'download'),
      ):
         with self.subTest(script=script_name):
            result, original, restored, java_command = self.run_crowdin_helper(script_name)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(restored, original)
            self.assertEqual(java_command, expected_command)
            self.assertNotIn(SYNTHETIC_CREDENTIAL, result.stdout + result.stderr)


if __name__ == '__main__':
   unittest.main()
