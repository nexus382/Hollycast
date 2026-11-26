#!/usr/bin/env python3

import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

import core_option_translation as t


def main():
   if len(sys.argv) < 3:
      print('Please provide Crowdin API Token and core name!')
      return 2

   api_key = sys.argv[1]
   core_name = t.clean_file_name(sys.argv[2])
   dir_path = t.os.path.dirname(t.os.path.realpath(__file__))
   yaml_path = t.os.path.join(dir_path, 'crowdin.yaml')

   try:
      with open(yaml_path, 'rb') as crowdin_config_file:
         original_config = crowdin_config_file.read()
   except OSError:
      print('Crowdin translation download failed', file=sys.stderr)
      return 1

   status = 0
   try:
      crowdin_config = original_config.decode('utf-8')
      crowdin_config = crowdin_config.replace(
         '"api_token": "_secret_"', f'"api_token": "{api_key}"', 1)
      crowdin_config = crowdin_config.replace('/_core_name_', f'/{core_name}')
      with open(yaml_path, 'w', encoding='utf-8', newline='') as crowdin_config_file:
         crowdin_config_file.write(crowdin_config)

      jar_name = 'crowdin-cli.jar'
      jar_path = t.os.path.join(dir_path, jar_name)
      crowdin_cli_file = 'crowdin-cli.zip'
      crowdin_cli_url = 'https://downloads.crowdin.com/cli/v3/' + crowdin_cli_file
      crowdin_cli_path = t.os.path.join(dir_path, crowdin_cli_file)

      if not os.path.isfile(jar_path):
         print('download crowdin-cli.jar')
         urllib.request.urlretrieve(crowdin_cli_url, crowdin_cli_path)
         with zipfile.ZipFile(crowdin_cli_path, 'r') as zip_ref:
            jar_dir = t.os.path.join(dir_path, zip_ref.namelist()[0])
            for file in zip_ref.namelist():
               if file.endswith(jar_name):
                  jar_file = file
                  break
            zip_ref.extract(jar_file, path=dir_path)
            os.rename(t.os.path.join(dir_path, jar_file), jar_path)
            os.remove(crowdin_cli_path)
            shutil.rmtree(jar_dir)

      print('download translation *.json')
      result = subprocess.run(['java', '-jar', jar_path, 'download', '--config', yaml_path])
      if result.returncode != 0:
         print(f'Crowdin translation download failed (exit {result.returncode})', file=sys.stderr)
         status = result.returncode if result.returncode > 0 else 1
   except Exception:
      print('Crowdin translation download failed', file=sys.stderr)
      status = 1
   finally:
      try:
         with open(yaml_path, 'wb') as crowdin_config_file:
            crowdin_config_file.write(original_config)
      except OSError:
         print('Crowdin translation download could not restore configuration', file=sys.stderr)
         status = 1

   return status


if __name__ == '__main__':
   sys.exit(main())
