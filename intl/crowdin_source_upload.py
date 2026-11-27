#!/usr/bin/env python3

from crowdin_cli import run_crowdin


if __name__ == '__main__':
   raise SystemExit(run_crowdin('source upload', 'upload', 'sources'))
