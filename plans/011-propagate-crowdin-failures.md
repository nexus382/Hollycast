# 011. Propagate Crowdin failures through both workflow layers

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 11 | P2 | S | Low | Translation tooling | `ebf8fbb79` · 2026-11-26 |

Both Python workflow wrappers returned 0 with every child reporting 1 in an isolated check. The inner Java invocations also ignore nonzero status. CI needs the first failed stage to end the command before regeneration or publication can proceed.

## Before starting

Work from the Hollycast repository root. Branch from `dev` as `fix/crowdin-exit-status`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'intl/upload_workflow.py' \
  'intl/download_workflow.py' \
  'intl/crowdin_source_upload.py' \
  'intl/crowdin_translation_download.py' \
  'intl/crowdin_cli.py' \
  'intl/tests/test_workflow_failures.py'
```

## Current behavior and local conventions

The download wrapper runs all three stages without checking their status. [intl/download_workflow.py:14](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/intl/download_workflow.py#L14)

```python
subprocess.run(['python3', 'intl/crowdin_prep.py', dir_path, core_name])
subprocess.run(['python3', 'intl/crowdin_translation_download.py', api_key, core_name])
subprocess.run(['python3', 'intl/crowdin_translate.py', dir_path, core_name])
```

The upload wrapper has the same behavior. [intl/upload_workflow.py:14](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/intl/upload_workflow.py#L14)

```python
subprocess.run(['python3', 'intl/crowdin_prep.py', dir_path, core_name])
subprocess.run(['python3', 'intl/crowdin_source_upload.py', api_key, core_name])
```

A successful Python invocation can conceal a failed Java download. [intl/crowdin_translation_download.py:60](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/intl/crowdin_translation_download.py#L60)

```python
      print('download translation *.json')
      subprocess.run(['java', '-jar', jar_path, 'download', '--config', YAML_PATH])
```

The upload helper also invokes Java directly. The surrounding helpers already attempt to restore their temporary configuration. [intl/crowdin_source_upload.py:60](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/intl/crowdin_source_upload.py#L60)

```python
      print('upload source *.json')
      subprocess.run(['java', '-jar', jar_path, 'upload', 'sources', '--config', YAML_PATH])
```

## Scope

- `intl/upload_workflow.py`
- `intl/download_workflow.py`
- `intl/crowdin_source_upload.py`
- `intl/crowdin_translation_download.py`
- `intl/crowdin_cli.py` (added during review to share stage handling and per-invocation configuration)
- `intl/tests/test_workflow_failures.py` (new)

Preserve the command-line arguments, source paths, Java flags, Crowdin project selection and checked-in translations. Exercise mocked subprocesses and a temporary working copy of configuration.

## Implementation and verification

### 1. Capture the wrapper contract offline

Add `WorkflowFailureTest` using Python’s unittest, a controlled argv, and mocked `subprocess.run`. Model each child exit independently. Assert the exact stage order, return status and the count of stages started after the first error. Include a unique synthetic argument that the test checks is absent from all diagnostics.

Run the **Syntax** command below. All four entry points and the shared module parse.

### 2. Fail fast through both layers

Check every child result, including Java inside the upload/download helpers. Use a numeric exit status and sanitized stage name in errors. Do not leak argv through an uncaught CalledProcessError, because wrapper arguments contain a credential. Preserve the original config contents through success, child failure or Python exception, using finally or isolated temporary configuration. Resolve relative CLI paths consistently if a temporary file is used.

Run the **Offline suite** command below. The new offline suite runs and every case passes.

### 3. Verify orchestration end to end without a service

Run the real wrapper process against local fake stage commands that return controlled success/failure results. Test both upload and download, and ensure later stages cannot run following a failed helper. Verify the checked-in translation inputs and config have no diff from the exercise.

Run the **Workflow contract** command below. The named integration case passes and the real config and generated header have no changes.

## Test plan

- Failures in preparation, upload, download and translation propagate. Verify successful children run once, in order, and all-success exits zero.
- Model a Java nonzero exit and a Java process-start error. Check the original YAML is restored byte for byte in both cases and diagnostic output excludes the synthetic argument.
- Run subprocess-level cases with no network or real credentials. Isolate scripts that derive config locations from `__file__` inside the temporary fixture.
- Overlap upload and download for two cores. Each Java process receives its own configuration and credential. The checked-in template stays byte-identical throughout.

## Verification commands

**Syntax**

```sh
python3 -B -c 'import ast, pathlib; names = ("upload_workflow", "download_workflow", "crowdin_source_upload", "crowdin_translation_download", "crowdin_cli"); [ast.parse((pathlib.Path("intl") / (name + ".py")).read_text()) for name in names]'
```

All five files parse.

**Offline suite**

```sh
python3 -B -m unittest -v intl.tests.test_workflow_failures
```

The new offline suite runs and every case passes.

**Workflow contract**

```sh
python3 -B -m unittest -v intl.tests.test_workflow_failures.WorkflowFailureTest.test_failed_prep_stops_pipeline && git diff --exit-code -- intl/crowdin.yaml shell/libretro/libretro_core_options_intl.h
```

The named integration case passes and the real config and generated header have no changes.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] Both actual wrapper commands return nonzero on failed stages, skip later work and succeed for all-success sequences. The four scripts and their shared module pass syntax checks. The offline suite verifies redaction and cleanup.
- [x] The step gates pass on macOS arm64. Crowdin CLI 3.15.0 accepts the generated configuration with a synthetic token supplied through the process environment.
- [x] The scoped diff covers the six listed files. The [plan index](README.md) records the accepted commit and independent test results.

## Independent review

Reviewed `704876809`. All seven offline workflow tests passed on macOS and Ubuntu. Both wrapper processes returned the failing child's exit code 27 and reported an interrupted child in extra subprocess checks. Crowdin CLI 3.15.0 validated the generated configuration offline. The template and translation header matched the baseline after testing.

## Stop conditions

If raising a subprocess exception includes the argument list in a traceback, return a sanitized error instead. Tests must never read developer credentials or change the real translation project. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Any new Crowdin stage should be covered by the same first-failure and credential-redaction tests. The GitHub job already respects a nonzero wrapper exit.
