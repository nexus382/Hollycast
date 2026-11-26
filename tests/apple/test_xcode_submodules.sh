#!/bin/bash

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
helper="$repo_root/shell/apple/init_submodules.sh"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/hollycast-xcode-submodules.XXXXXX")"
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM
real_git="$(command -v git)"

export GIT_CONFIG_GLOBAL="$fixture_root/gitconfig"
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME="Hollycast Fixture"
export GIT_AUTHOR_EMAIL="fixture@example.invalid"
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
git config --file "$GIT_CONFIG_GLOBAL" user.name "$GIT_AUTHOR_NAME"
git config --file "$GIT_CONFIG_GLOBAL" user.email "$GIT_AUTHOR_EMAIL"
git config --file "$GIT_CONFIG_GLOBAL" init.defaultBranch master
git config --file "$GIT_CONFIG_GLOBAL" protocol.file.allow always

source "$helper"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

assert_equal() {
	local expected="$1" actual="$2" description="$3"
	if [ "$expected" != "$actual" ]; then
		echo "FAIL: $description" >&2
		echo "  expected: $expected" >&2
		echo "  actual:   $actual" >&2
		exit 1
	fi
}

run_helper() {
	local root="$1"
	(
		cd "$root"
		init_submodules
	)
}

run_helper_monitored() {
	local root="$1"
	(
		cd "$root"
		PATH="$monitor_bin:$PATH" GIT_REAL="$real_git" GIT_MUTATION_LOG="$mutation_log" init_submodules
	)
}

checkout_identity() {
	local root="$1" branch
	branch="$(git -C "$root" symbolic-ref --quiet --short HEAD || true)"
	if [ -n "$branch" ]; then
		printf 'branch:%s' "$branch"
	else
		printf 'detached'
	fi
}

assert_submodule_pin() {
	local root="$1" expected actual
	expected="$(git -C "$root" rev-parse HEAD:core/deps/glslang)"
	actual="$(git -C "$root/core/deps/glslang" rev-parse HEAD)"
	assert_equal "$expected" "$actual" "submodule pin at $root"
	[ -f "$root/core/deps/glslang/CMakeLists.txt" ] || fail "submodule files missing at $root"
}

submodule_origin="$fixture_root/glslang origin.git"
super_origin="$fixture_root/Hollycast origin.git"
submodule_seed="$fixture_root/glslang seed"
super_seed="$fixture_root/Hollycast seed"

git init --bare "$submodule_origin" >/dev/null
git clone -q "$submodule_origin" "$submodule_seed"
printf 'cmake_minimum_required(VERSION 3.10)\nproject(glslang_fixture)\n' > "$submodule_seed/CMakeLists.txt"
git -C "$submodule_seed" add CMakeLists.txt
git -C "$submodule_seed" commit -m 'Add fixture submodule content' >/dev/null
git -C "$submodule_seed" push -u origin master >/dev/null

git init --bare "$super_origin" >/dev/null
git init "$super_seed" >/dev/null
mkdir -p "$super_seed/core/deps"
printf 'tracked sentinel bytes\000with nul\n' > "$super_seed/sentinel.bin"
git -C "$super_seed" add sentinel.bin
git -C "$super_seed" commit -m 'Add fixture sentinel' >/dev/null
git -C "$super_seed" -c protocol.file.allow=always submodule add "$submodule_origin" core/deps/glslang >/dev/null
git -C "$super_seed" commit -m 'Pin fixture submodule' >/dev/null
git -C "$super_seed" switch -c extra-fixture-commit >/dev/null
printf 'extra branch commit\n' > "$super_seed/extra-branch.txt"
git -C "$super_seed" add extra-branch.txt
git -C "$super_seed" commit -m 'Add extra fixture branch commit' >/dev/null
extra_commit="$(git -C "$super_seed" rev-parse HEAD)"
git -C "$super_seed" remote add origin "$super_origin"
git -C "$super_seed" push -u origin master extra-fixture-commit >/dev/null

normal="$fixture_root/normal checkout"
linked="$fixture_root/linked checkout with spaces"
git clone -q "$super_origin" "$normal"
git -C "$normal" fetch -q origin extra-fixture-commit
git -C "$normal" worktree add --detach "$linked" "$extra_commit" >/dev/null

[ -d "$normal/.git" ] || fail 'normal checkout does not have a .git directory'
[ -f "$linked/.git" ] || fail 'linked checkout does not have a .git file'

monitor_bin="$fixture_root/monitor-bin"
mutation_log="$fixture_root/checkout-mutations.log"
mkdir -p "$monitor_bin"
cat > "$monitor_bin/git" <<'MONITOR'
#!/bin/bash
case " $* " in
	*' init '*) printf 'init\n' >> "$GIT_MUTATION_LOG" ;;
	*' remote add '*) printf 'remote add\n' >> "$GIT_MUTATION_LOG" ;;
	*' fetch --depth 1 origin master '*) printf 'fetch master\n' >> "$GIT_MUTATION_LOG" ;;
	*' reset --hard FETCH_HEAD '*) printf 'reset\n' >> "$GIT_MUTATION_LOG" ;;
esac
exec "$GIT_REAL" "$@"
MONITOR
chmod +x "$monitor_bin/git"
: > "$mutation_log"

for checkout in "$normal" "$linked"; do
	if [ -f "$checkout/core/deps/glslang/CMakeLists.txt" ]; then
		fail "submodule was already populated in fixture $checkout"
	fi
	dir_before="$(git -C "$checkout" rev-parse HEAD)"
	branch_before="$(checkout_identity "$checkout")"
	remotes_before="$(git -C "$checkout" remote -v)"
	cmp -s "$checkout/sentinel.bin" "$super_seed/sentinel.bin" || fail "sentinel differs before helper at $checkout"

	run_helper_monitored "$checkout"
	[ ! -s "$mutation_log" ] || fail "archive bootstrap Git commands ran in $checkout: $(cat "$mutation_log")"
	assert_equal "$dir_before" "$(git -C "$checkout" rev-parse HEAD)" "HEAD changed at $checkout"
	assert_equal "$branch_before" "$(checkout_identity "$checkout")" "branch or detached state changed at $checkout"
	assert_equal "$remotes_before" "$(git -C "$checkout" remote -v)" "remotes changed at $checkout"
	assert_submodule_pin "$checkout"
	cmp -s "$checkout/sentinel.bin" "$super_seed/sentinel.bin" || fail "sentinel bytes changed at $checkout"

	run_helper_monitored "$checkout"
	[ ! -s "$mutation_log" ] || fail "archive bootstrap Git commands ran on repeat in $checkout: $(cat "$mutation_log")"
	assert_equal "$dir_before" "$(git -C "$checkout" rev-parse HEAD)" "HEAD changed on repeat at $checkout"
	assert_submodule_pin "$checkout"
	cmp -s "$checkout/sentinel.bin" "$super_seed/sentinel.bin" || fail "sentinel bytes changed on repeat at $checkout"
done
assert_equal "$extra_commit" "$(git -C "$linked" rev-parse HEAD)" 'linked worktree did not retain its extra commit'
echo 'PASS: normal and linked worktrees initialize pinned submodules and remain unchanged'

# Exercise the existing unborn-repository bootstrap against the local fixture origin.
unborn="$fixture_root/unborn checkout"
git init "$unborn" >/dev/null
git -C "$unborn" remote add origin "$super_origin"
unborn_branch_before="$(checkout_identity "$unborn")"
unborn_remotes_before="$(git -C "$unborn" remote -v)"
run_helper "$unborn"
assert_equal "$unborn_branch_before" "$(checkout_identity "$unborn")" 'unborn branch changed during bootstrap'
assert_equal "$unborn_remotes_before" "$(git -C "$unborn" remote -v)" 'unborn checkout remotes changed during bootstrap'
assert_equal "$(git -C "$super_origin" rev-parse refs/heads/master)" "$(git -C "$unborn" rev-parse HEAD)" 'unborn checkout did not fetch the local master snapshot'
assert_submodule_pin "$unborn"
echo 'PASS: valid unborn checkout retains its branch and uses the local bootstrap remote'

# The source-archive branch is observed with a recorder so destructive commands never run.
archive="$fixture_root/archive checkout"
mkdir -p "$archive"
archive="$(cd "$archive" && pwd -P)"
recorder_bin="$fixture_root/recorder-bin"
mkdir -p "$recorder_bin"
real_path="$PATH"
cat > "$recorder_bin/git" <<'RECORDER'
#!/bin/bash
case " $* " in
	*' rev-parse --is-inside-work-tree '*) exit 128 ;;
	*' rev-parse --show-toplevel '*) exit 128 ;;
	*' init '*)
		printf '%s\n' "$*" >> "$GIT_COMMAND_RECORDER"
		;;
	*' remote add origin https://github.com/nexus382/Hollycast.git '*)
		printf '%s\n' "$*" >> "$GIT_COMMAND_RECORDER"
		;;
	*' fetch --depth 1 origin master '*)
		printf '%s\n' "$*" >> "$GIT_COMMAND_RECORDER"
		;;
	*' reset --hard FETCH_HEAD '*)
		printf '%s\n' "$*" >> "$GIT_COMMAND_RECORDER"
		;;
	*' submodule update --init --recursive '*)
		printf '%s\n' "$*" >> "$GIT_COMMAND_RECORDER"
		;;
	*)
		echo "Unexpected Git command in archive recorder: $*" >&2
		exit 99
		;;
esac
RECORDER
chmod +x "$recorder_bin/git"
export GIT_COMMAND_RECORDER="$fixture_root/archive-commands.log"
: > "$GIT_COMMAND_RECORDER"
expected_archive_log="$fixture_root/archive-commands.expected"
cat > "$expected_archive_log" <<'EXPECTED'
-C ARCHIVE_ROOT init
-C ARCHIVE_ROOT remote add origin https://github.com/nexus382/Hollycast.git
-C ARCHIVE_ROOT fetch --depth 1 origin master
-C ARCHIVE_ROOT reset --hard FETCH_HEAD
-C ARCHIVE_ROOT submodule update --init --recursive
EXPECTED
if PATH="$recorder_bin:$real_path" ARCHIVE_ROOT="$archive" bash -c 'cd "$ARCHIVE_ROOT"; source "$1"; init_submodules' _ "$helper"; then
	:
else
	fail 'archive bootstrap classifier failed'
fi
sed "s|ARCHIVE_ROOT|$archive|g" "$expected_archive_log" > "$fixture_root/archive-commands.expected.actual"
cmp -s "$fixture_root/archive-commands.expected.actual" "$GIT_COMMAND_RECORDER" || {
	echo 'FAIL: source archive did not select only the historical bootstrap commands' >&2
	cat "$GIT_COMMAND_RECORDER" >&2
	exit 1
}
[ ! -e "$archive/.git" ] || fail 'archive recorder unexpectedly created Git metadata'
echo 'PASS: source archive classification reaches only the consented bootstrap sequence'

# An extracted folder inside another repository must not bootstrap or mutate its parent.
enclosing="$fixture_root/unrelated enclosing repository"
nested="$enclosing/extracted project"
git init "$enclosing" >/dev/null
printf 'outer sentinel\n' > "$enclosing/sentinel.txt"
git -C "$enclosing" add sentinel.txt
git -C "$enclosing" commit -m 'Create unrelated enclosing repository' >/dev/null
git -C "$enclosing" remote add preserved-origin "$fixture_root/not-a-real-remote.git"
mkdir -p "$nested"
printf 'nested archive data\n' > "$nested/README.txt"
enclosing_head_before="$(git -C "$enclosing" rev-parse HEAD)"
enclosing_remotes_before="$(git -C "$enclosing" remote -v)"
if run_helper "$nested" 2> "$fixture_root/enclosing-error.log"; then
	fail 'nested archive under unrelated repository was accepted'
fi
assert_equal "$enclosing_head_before" "$(git -C "$enclosing" rev-parse HEAD)" 'enclosing repository HEAD changed'
assert_equal "$enclosing_remotes_before" "$(git -C "$enclosing" remote -v)" 'enclosing repository remotes changed'
assert_equal 'outer sentinel' "$(cat "$enclosing/sentinel.txt")" 'enclosing repository sentinel changed'
[ ! -e "$nested/.git" ] || fail 'nested archive gained Git metadata'
grep -q 'unrelated repository root' "$fixture_root/enclosing-error.log" || fail 'nested archive error did not identify the unrelated root'
echo 'PASS: unrelated enclosing repository is rejected without mutation'

invalid="$fixture_root/invalid gitfile"
mkdir -p "$invalid"
printf 'gitdir: %s\n' "$fixture_root/missing-git-metadata" > "$invalid/.git"
invalid_gitfile_before="$(cat "$invalid/.git")"
if run_helper "$invalid" 2> "$fixture_root/invalid-error.log"; then
	fail 'invalid gitfile was accepted'
fi
assert_equal "$invalid_gitfile_before" "$(cat "$invalid/.git")" 'invalid gitfile was changed'
[ ! -e "$invalid/HEAD" ] || fail 'invalid gitfile directory was initialized as a repository'
grep -q 'Git metadata.*invalid' "$fixture_root/invalid-error.log" || fail 'invalid gitfile error was unclear'
echo 'PASS: corrupt gitfile is rejected before bootstrap mutation'

echo 'All Xcode submodule bootstrap fixtures passed.'
