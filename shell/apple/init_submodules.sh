# Shared by the Xcode generator and the offline submodule bootstrap tests.
init_submodules() {
	local project_root git_dir_present git_inside git_top canonical_git_top

	project_root="$(pwd -P)"
	if [ -e "$project_root/.git" ] || [ -L "$project_root/.git" ]; then
		git_dir_present=yes
	else
		git_dir_present=no
	fi

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

		if git -C "$project_root" rev-parse --verify HEAD >/dev/null 2>&1; then
			git -C "$project_root" submodule update --init --recursive
		else
			git -C "$project_root" fetch --depth 1 origin master
			git -C "$project_root" reset --hard FETCH_HEAD
			git -C "$project_root" submodule update --init --recursive
		fi
		return
	fi

	if [ "$git_dir_present" = yes ]; then
		echo "Cannot initialize submodules: Git metadata at $project_root is invalid." >&2
		return 1
	fi

	# Source archives have no Git metadata. Keep their historical, consented setup.
	git -C "$project_root" init
	git -C "$project_root" remote add origin https://github.com/nexus382/Hollycast.git
	git -C "$project_root" fetch --depth 1 origin master
	git -C "$project_root" reset --hard FETCH_HEAD
	git -C "$project_root" submodule update --init --recursive
}
