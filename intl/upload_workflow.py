#!/usr/bin/env python3

import sys

from crowdin_cli import run_stage


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
