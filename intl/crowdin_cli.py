import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

from core_option_translation import clean_file_name


def run_stage(stage, command, *, env=None):
   try:
      status = subprocess.run(command, env=env).returncode
   except OSError:
      print(f'{stage} failed to start', file=sys.stderr)
      return 1
   if status:
      print(f'{stage} failed (exit {status})', file=sys.stderr)
   return status if status >= 0 else 1


def find_cli(directory):
   jar = directory / 'crowdin-cli.jar'
   if jar.is_file():
      return jar

   print('download crowdin-cli.jar')
   with tempfile.TemporaryDirectory(dir=directory) as temporary:
      work = Path(temporary)
      archive = work / 'crowdin-cli.zip'
      urllib.request.urlretrieve('https://downloads.crowdin.com/cli/v3/crowdin-cli.zip', archive)
      with zipfile.ZipFile(archive) as zipped:
         member = next(entry for entry in zipped.infolist()
                       if not entry.is_dir() and Path(entry.filename).name == jar.name)
         staged_jar = work / jar.name
         with zipped.open(member) as source, staged_jar.open('wb') as destination:
            shutil.copyfileobj(source, destination)
      try:
         os.replace(staged_jar, jar)
      except PermissionError:
         # Windows can lock a JAR already published and opened by another run.
         if not jar.is_file():
            raise
   return jar


def run_crowdin(stage, *arguments):
   if len(sys.argv) < 3:
      print('Please provide Crowdin API Token and core name!', file=sys.stderr)
      return 2

   directory = Path(__file__).resolve().parent
   try:
      core_name = clean_file_name(sys.argv[2])
      template = (directory / 'crowdin.yaml').read_text(encoding='utf-8')
      # Use the supplied token only in the CLI process. Keep file globs relative
      # to the same directory as the project template.
      config = re.sub(
         r'''(?m)^[ \t]*["']?api_token(?:_env)?["']?[ \t]*:.*$''',
         '', template)
      config += '\n"api_token_env": "CROWDIN_PERSONAL_TOKEN"\n'
      config = config.replace('/_core_name_', f'/{core_name}')
      jar = find_cli(directory)
      private_config = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
            dir=directory, prefix='.crowdin-', suffix='.yaml', delete=False)
      try:
         with private_config:
            private_config.write(config)
         print(f'Crowdin {stage}')
         command = ['java', '-jar', str(jar), *arguments, '--config', private_config.name]
         environment = {**os.environ, 'CROWDIN_PERSONAL_TOKEN': sys.argv[1]}
         return run_stage(f'Crowdin {stage}', command, env=environment)
      finally:
         os.unlink(private_config.name)
   except Exception:
      print(f'Crowdin {stage} failed', file=sys.stderr)
      return 1
