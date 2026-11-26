# 015. Recognize linked worktrees in the Xcode project bootstrap

| Audit finding | Priority | Effort | Risk of change | Area | Planned at |
| --- | --- | --- | --- | --- | --- |
| 15 | P2 | S | Low | macOS developer tooling | `ebf8fbb79` · 2026-11-26 |

The Xcode generator tests whether `.git` is a directory. A normal linked worktree has a `.git` file, so initializing missing submodules takes the source-archive path and tries to initialize Git and add an existing remote. Preserve the developer’s active branch and commit.

## Before starting

Work from the Hollycast repository root. Branch from `dev` as `fix/xcode-linked-worktree-bootstrap`. Keep the change in one logical commit or a short series with imperative subjects. The target branch for an authorized pull request is `dev`.

This plan can start independently. Compare the scoped history against the baseline. If a hunk has moved, re-find its symbol and check that the behavior below still applies.

```sh
git diff --stat ebf8fbb79..HEAD -- \
  'shell/apple/generate_xcode_project.command' \
  'shell/apple/init_submodules.sh' \
  'tests/apple/test_xcode_submodules.sh'
```

## Current behavior and local conventions

A valid checkout with a HEAD needs only recursive initialization of its pinned submodules. [shell/apple/generate_xcode_project.command:41](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/shell/apple/generate_xcode_project.command#L41)

```bash
init_submodules() {
    if [ -d .git ]; then
        if git rev-parse --verify HEAD >/dev/null 2>&1; then
            git submodule update --init --recursive
        else
```

The alternate branch initializes a repository and switches to a fetched master snapshot. [shell/apple/generate_xcode_project.command:50](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/shell/apple/generate_xcode_project.command#L50)

```bash
    else
        git init
        git remote add origin https://github.com/nexus382/Hollycast.git
        git fetch --depth 1 origin master
        git reset --hard FETCH_HEAD
        git submodule update --init --recursive
```

Initialization is prompted only when the glslang submodule is missing. [shell/apple/generate_xcode_project.command:179](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/shell/apple/generate_xcode_project.command#L179)

```bash
if [ ! -f core/deps/glslang/CMakeLists.txt ]; then
    echo 'Git submodules are missing.' >&2
    read -p $'Type \033[1;32my\033[0m to initialize git and fetch submodules recursively (ZIP checkout files will be reset to the upstream master snapshot): ' g
    if [ "$g" = "y" ]; then
        init_submodules
    else
```

The Windows harness also asks Git for worktree metadata instead of deriving its shape. [tools/testing/build-windows.ps1:247](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tools/testing/build-windows.ps1#L247)

```cpp
        $commonGitDirectoryLines = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir'))
```

## Scope

- `shell/apple/generate_xcode_project.command`
- `shell/apple/init_submodules.sh` (new)
- `tests/apple/test_xcode_submodules.sh` (new)

Keep the macOS/iOS choices, Homebrew and Xcode setup, MoltenVK selection, existing consent prompt and intentionally supported archive bootstrap. Test the submodule routine in isolation from the interactive installer.

## Implementation and verification

### 1. Isolate the existing Git routine

Put `init_submodules` in a small Bash file sourced by the generator from its own script directory so the test can invoke the exact production logic. Keep a single implementation. If the routine is already sourceable after drift, reuse it. Check the generator and helper with macOS Bash.

Run the **Syntax** command below. Both production shell files parse with Bash.

### 2. Detect the actual checkout root

Check `git rev-parse --is-inside-work-tree` and the canonical `--show-toplevel` against the project root. Treat a committed primary checkout and a committed linked checkout identically. Preserve deliberate handling of an unborn checkout and the consented source-archive case. A damaged gitfile or an unrelated enclosing repository must yield a clear error before any reset or remote mutation.

Run the **Fixture suite** command below. The offline fixtures report success for linked and main checkouts, archive classification, and invalid/enclosing Git metadata. All temporary repositories are removed on exit.

### 3. Test a linked checkout with missing submodules

In a temporary directory create a local origin containing a real submodule and an extra local branch commit. Create a detached linked worktree at that commit, run only the helper, then compare HEAD, branch/detached state, remote list and a tracked sentinel before and after. Verify the pinned submodule commit was populated. Keep all fixture configuration and transport overrides local to the test process.

Run the **Fixture suite** command below. The offline fixtures report success for linked and main checkouts, archive classification, and invalid/enclosing Git metadata. All temporary repositories are removed on exit.

## Test plan

- Run against a real linked worktree whose `.git` is a file, and a normal clone. Include a path containing spaces. Ensure the helper initializes the submodule at the superproject gitlink and a second run is harmless.
- Exercise the archive and unborn cases in disposable roots with a local fake upstream or a command recorder. For a linked checkout assert there are no `init`, remote-add, fetch-master or reset invocations.
- Test an extracted folder nested under an unrelated repository and an invalid gitfile. Verify the enclosing repository’s HEAD, remotes and tracked sentinel remain unchanged. Use fixture-scoped Git identity and file-transport configuration.
- Check a malformed branch reference and a reference to a missing commit. A failed fetch must return its exit code before reset, including when the helper is sourced without Bash’s exit-on-error option.

## Verification commands

**Syntax**

```sh
bash -n shell/apple/generate_xcode_project.command && bash -n shell/apple/init_submodules.sh
```

Both production shell files parse with Bash.

**Fixture suite**

```sh
bash -n tests/apple/test_xcode_submodules.sh && bash tests/apple/test_xcode_submodules.sh
```

The offline fixtures report success for linked and main checkouts, archive classification, and invalid/enclosing Git metadata. All temporary repositories are removed on exit.

**Scope**

```sh
git diff --check && git diff --cached --check && git status --short
```

Both whitespace checks exit zero. Compare the change list with the scope below.

## Done criteria

- [x] A real linked checkout retained its commit, detached state, remotes and tracked sentinel through two runs. Its submodule matched the superproject gitlink.
- [x] The shell fixtures passed on macOS arm64 with Bash 3.2.57. Independent generator checks preserved staged and unstaged edits, propagated a submodule failure and completed an archive bootstrap against a local upstream.
- [x] The reviewed diff covers the three listed files. The [plan index](README.md) records the accepted commit.

## Independent review

Reviewed `d0ddf2406` with Bash 3.2 on macOS. The fixtures checked the pinned commit and a repeated run in linked and ordinary worktrees. Running the generator through a symlink preserved staged and unstaged edits. An injected update failure returned exit 23. A local archive bootstrap reached its expected commit, and both an invalid branch reference and a failed fetch stopped before reset.

## Stop conditions

If Git resolves the project to an unrelated top-level directory or cannot parse existing metadata, leave the checkout intact and report that condition. The real generator should never be run against the user’s development checkout as a bootstrap test. If upstream has already fixed the behavior, demonstrate it with the regression before retiring the plan.

## Maintenance notes

Use Git’s worktree discovery when adding future bootstrap branches. Keep all destructive archive setup behind its existing consent and outside the recognized-worktree path.
