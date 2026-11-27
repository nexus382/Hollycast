# Shared by the Xcode generator and the offline submodule bootstrap tests.
init_submodules() {
	local project_root git_inside git_top canonical_git_top head_ref
	project_root="$(pwd -P)" || return

	if git_inside="$(git -C "$project_root" rev-parse --is-inside-work-tree 2>/dev/null)"; then
		if [ "$git_inside" != true ]; then
			echo "Cannot initialize submodules: $project_root is not a Git worktree." >&2
			return 1
		fi

		if ! git_top="$(git -C "$project_root" rev-parse --show-toplevel 2>/dev/null)" ||
			! canonical_git_top="$(cd "$git_top" 2>/dev/null && pwd -P)"; then
			echo "Cannot initialize submodules: Git metadata at $project_root is invalid." >&2
			return 1
		fi
		if [ "$canonical_git_top" != "$project_root" ]; then
			echo "Cannot initialize submodules: Git resolved $project_root to unrelated repository root $canonical_git_top." >&2
			return 1
		fi

		if git -C "$project_root" rev-parse --verify 'HEAD^{commit}' >/dev/null 2>&1; then
			git -C "$project_root" submodule update --init --recursive
			return
		fi

		# An unborn branch has a symbolic HEAD and no object ID yet.
		head_ref="$(git -C "$project_root" symbolic-ref --quiet HEAD 2>/dev/null)" || head_ref=
		if [[ "$head_ref" != refs/heads/* ]] || git -C "$project_root" rev-parse --verify HEAD >/dev/null 2>&1; then
			echo "Cannot initialize submodules because HEAD at $project_root is damaged." >&2
			return 1
		fi
	elif [ -e "$project_root/.git" ] || [ -L "$project_root/.git" ]; then
		echo "Cannot initialize submodules: Git metadata at $project_root is invalid." >&2
		return 1
	else
		# Source archives retain the setup approved in the generator.
		git -C "$project_root" init || return
		git -C "$project_root" remote add origin https://github.com/nexus382/Hollycast.git || return
	fi

	git -C "$project_root" fetch --depth 1 origin master || return
	git -C "$project_root" reset --hard FETCH_HEAD || return
	git -C "$project_root" submodule update --init --recursive
}
