#!/usr/bin/env python3

import subprocess
import sys


def run_stage(stage, command):
   try:
      result = subprocess.run(command)
   except OSError:
      print(f'{stage} failed to start', file=sys.stderr)
      return 1

   if result.returncode != 0:
      print(f'{stage} failed (exit {result.returncode})', file=sys.stderr)
      return result.returncode if result.returncode > 0 else 1
   return 0


def main():
   if len(sys.argv) < 4:
      print('Please provide path to libretro_core_options.h, Crowdin API Token and core name!')
      return 2

   api_key = sys.argv[1]
   core_name = sys.argv[2]
   dir_path = sys.argv[3]

   status = run_stage('preparation', ['python3', 'intl/crowdin_prep.py', dir_path, core_name])
   if status:
      return status

   return run_stage('source upload', ['python3', 'intl/crowdin_source_upload.py', api_key, core_name])


if __name__ == '__main__':
   sys.exit(main())
