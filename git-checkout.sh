#!/bin/sh

USAGE='[-f] [-b <new_branch>] [<branch>] [<paths>...]'
. git-sh-setup

old=$(git-rev-parse HEAD)
new=
force=
branch=
newbranch=
while [ "$#" != "0" ]; do
    arg="$1"
    shift
    case "$arg" in
	"-b")
		newbranch="$1"
		shift
		[ -z "$newbranch" ] &&
			die "git checkout: -b needs a branch name"
		[ -e "$GIT_DIR/refs/heads/$newbranch" ] &&
			die "git checkout: branch $newbranch already exists"
		git-check-ref-format "heads/$newbranch" ||
			die "we do not like '$newbranch' as a branch name."
		;;
	"-f")
		force=1
		;;
	--)
		break
		;;
	-*)
		usage
		;;
	*)
		if rev=$(git-rev-parse --verify "$arg^0" 2>/dev/null)
		then
			if [ -z "$rev" ]; then
				echo "unknown flag $arg"
				exit 1
			fi
			new="$rev"
			if [ -f "$GIT_DIR/refs/heads/$arg" ]; then
				branch="$arg"
			fi
		elif rev=$(git-rev-parse --verify "$arg^{tree}" 2>/dev/null)
		then
			# checking out selected paths from a tree-ish.
			new="$rev"
			branch=
		else
			new=
			branch=
			set x "$arg" "$@"
			shift
		fi
		break
		;;
    esac
done

# The behaviour of the command with and without explicit path
# parameters is quite different.
#
# Without paths, we are checking out everything in the work tree,
# possibly switching branches.  This is the traditional behaviour.
#
# With paths, we are _never_ switching branch, but checking out
# the named paths from either index (when no rev is given),
# or the named tree-ish (when rev is given).

if test "$#" -ge 1
then
	if test '' != "$newbranch$force"
	then
		die "updating paths and switching branches or forcing are incompatible."
	fi
	if test '' != "$new"
	then
		# from a specific tree-ish; note that this is for
		# rescuing paths and is never meant to remove what
		# is not in the named tree-ish.
		git-ls-tree -r "$new" "$@" |
		git-update-index --index-info || exit $?
	fi
	git-checkout-index -f -u -- "$@"
	exit $?
else
	# Make sure we did not fall back on $arg^{tree} codepath
	# since we are not checking out from an arbitrary tree-ish,
	# but switching branches.
	if test '' != "$new"
	then
		git-rev-parse --verify "$new^{commit}" >/dev/null 2>&1 ||
		die "Cannot switch branch to a non-commit."
	fi
fi

[ -z "$new" ] && new=$old

# If we don't have an old branch that we're switching to,
# and we don't have a new branch name for the target we
# are switching to, then we'd better just be checking out
# what we already had

[ -z "$branch$newbranch" ] &&
	[ "$new" != "$old" ] &&
	die "git checkout: you need to specify a new branch name"

if [ "$force" ]
then
    git-read-tree --reset $new &&
	git-checkout-index -q -f -u -a
else
    git-update-index --refresh >/dev/null
    git-read-tree -m -u $old $new
fi

# 
# Switch the HEAD pointer to the new branch if we
# checked out a branch head, and remove any potential
# old MERGE_HEAD's (subsequent commits will clearly not
# be based on them, since we re-set the index)
#
if [ "$?" -eq 0 ]; then
	if [ "$newbranch" ]; then
		leading=`expr "refs/heads/$newbranch" : '\(.*\)/'` &&
		mkdir -p "$GIT_DIR/$leading" &&
		echo $new >"$GIT_DIR/refs/heads/$newbranch" || exit
		branch="$newbranch"
	fi
	[ "$branch" ] &&
	GIT_DIR="$GIT_DIR" git-symbolic-ref HEAD "refs/heads/$branch"
	rm -f "$GIT_DIR/MERGE_HEAD"
else
	exit 1
fi
