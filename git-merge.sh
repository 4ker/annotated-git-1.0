#!/bin/sh
#
# Copyright (c) 2005 Junio C Hamano
#


USAGE='[-n] [--no-commit] [-s <strategy>]... <merge-message> <head> <remote>+'
. git-sh-setup

LF='
'

all_strategies='recursive octopus resolve stupid ours'
default_strategies='recursive'
use_strategies=

dropsave() {
	rm -f -- "$GIT_DIR/MERGE_HEAD" "$GIT_DIR/MERGE_MSG" \
		 "$GIT_DIR/MERGE_SAVE" || exit 1
}

savestate() {
	# Stash away any local modifications.
	git-diff-index -z --name-only $head |
	cpio -0 -o >"$GIT_DIR/MERGE_SAVE"
}

restorestate() {
        if test -f "$GIT_DIR/MERGE_SAVE"
	then
		git reset --hard $head
		cpio -iuv <"$GIT_DIR/MERGE_SAVE"
		git-update-index --refresh >/dev/null
	fi
}

finish () {
	test '' = "$2" || echo "$2"
	case "$merge_msg" in
	'')
		echo "No merge message -- not updating HEAD"
		;;
	*)
		git-update-ref HEAD "$1" "$head" || exit 1
		;;
	esac

	case "$no_summary" in
	'')
		git-diff-tree -p -M "$head" "$1" |
		git-apply --stat --summary
		;;
	esac
}

while case "$#" in 0) break ;; esac
do
	case "$1" in
	-n|--n|--no|--no-|--no-s|--no-su|--no-sum|--no-summ|\
		--no-summa|--no-summar|--no-summary)
		no_summary=t ;;
	--no-c|--no-co|--no-com|--no-comm|--no-commi|--no-commit)
		no_commit=t ;;
	-s=*|--s=*|--st=*|--str=*|--stra=*|--strat=*|--strate=*|\
		--strateg=*|--strategy=*|\
	-s|--s|--st|--str|--stra|--strat|--strate|--strateg|--strategy)
		case "$#,$1" in
		*,*=*)
			strategy=`expr "$1" : '-[^=]*=\(.*\)'` ;;
		1,*)
			usage ;;
		*)
			strategy="$2"
			shift ;;
		esac
		case " $all_strategies " in
		*" $strategy "*)
			use_strategies="$use_strategies$strategy " ;;
		*)
			die "available strategies are: $all_strategies" ;;
		esac
		;;
	-*)	usage ;;
	*)	break ;;
	esac
	shift
done

test "$#" -le 2 && usage ;# we need at least two heads.

merge_msg="$1"
shift
head_arg="$1"
head=$(git-rev-parse --verify "$1"^0) || usage
shift

# All the rest are remote heads
remoteheads=
for remote
do
	remotehead=$(git-rev-parse --verify "$remote"^0) ||
	    die "$remote - not something we can merge"
	remoteheads="${remoteheads}$remotehead "
done
set x $remoteheads ; shift

case "$#" in
1)
	common=$(git-merge-base --all $head "$@")
	;;
*)
	common=$(git-show-branch --merge-base $head "$@")
	;;
esac
echo "$head" >"$GIT_DIR/ORIG_HEAD"

case "$#,$common,$no_commit" in
*,'',*)
	# No common ancestors found. We need a real merge.
	;;
1,"$1",*)
	# If head can reach all the merge then we are up to date.
	# but first the most common case of merging one remote
	echo "Already up-to-date."
	dropsave
	exit 0
	;;
1,"$head",*)
	# Again the most common case of merging one remote.
	echo "Updating from $head to $1."
	git-update-index --refresh 2>/dev/null
	new_head=$(git-rev-parse --verify "$1^0") &&
	git-read-tree -u -m $head "$new_head" &&
	finish "$new_head" "Fast forward"
	dropsave
	exit 0
	;;
1,?*"$LF"?*,*)
	# We are not doing octopus and not fast forward.  Need a
	# real merge.
	;;
1,*,)
	# We are not doing octopus, not fast forward, and have only
	# one common.  See if it is really trivial.
	echo "Trying really trivial in-index merge..."
	git-update-index --refresh 2>/dev/null
	if git-read-tree --trivial -m -u $common $head "$1" &&
	   result_tree=$(git-write-tree)
	then
	    echo "Wonderful."
	    result_commit=$(
	        echo "$merge_msg" |
	        git-commit-tree $result_tree -p HEAD -p "$1"
	    ) || exit
	    finish "$result_commit" "In-index merge"
	    dropsave
	    exit 0
	fi
	echo "Nope."
	;;
*)
	# An octopus.  If we can reach all the remote we are up to date.
	up_to_date=t
	for remote
	do
		common_one=$(git-merge-base --all $head $remote)
		if test "$common_one" != "$remote"
		then
			up_to_date=f
			break
		fi
	done
	if test "$up_to_date" = t
	then
		echo "Already up-to-date. Yeeah!"
		dropsave
		exit 0
	fi
	;;
esac

case "$use_strategies" in
'')
	case "$#" in
	1)
		use_strategies="$default_strategies" ;;
	*)
		use_strategies=octopus ;;
	esac		
	;;
esac

# At this point, we need a real merge.  No matter what strategy
# we use, it would operate on the index, possibly affecting the
# working tree, and when resolved cleanly, have the desired tree
# in the index -- this means that the index must be in sync with
# the $head commit.  The strategies are responsible to ensure this.

case "$use_strategies" in
?*' '?*)
    # Stash away the local changes so that we can try more than one.
    savestate
    single_strategy=no
    ;;
*)
    rm -f "$GIT_DIR/MERGE_SAVE"
    single_strategy=yes
    ;;
esac

result_tree= best_cnt=-1 best_strategy= wt_strategy=
for strategy in $use_strategies
do
    test "$wt_strategy" = '' || {
	echo "Rewinding the tree to pristine..."
	restorestate
    }
    case "$single_strategy" in
    no)
	echo "Trying merge strategy $strategy..."
	;;
    esac

    # Remember which strategy left the state in the working tree
    wt_strategy=$strategy

    git-merge-$strategy $common -- "$head_arg" "$@"
    exit=$?
    if test "$no_commit" = t && test "$exit" = 0
    then
	exit=1 ;# pretend it left conflicts.
    fi

    test "$exit" = 0 || {

	# The backend exits with 1 when conflicts are left to be resolved,
	# with 2 when it does not handle the given merge at all.

	if test "$exit" -eq 1
	then
	    cnt=`{
		git-diff-files --name-only
		git-ls-files --unmerged
	    } | wc -l`
	    if test $best_cnt -le 0 -o $cnt -le $best_cnt
	    then
		best_strategy=$strategy
		best_cnt=$cnt
	    fi
	fi
	continue
    }

    # Automerge succeeded.
    result_tree=$(git-write-tree) && break
done

# If we have a resulting tree, that means the strategy module
# auto resolved the merge cleanly.
if test '' != "$result_tree"
then
    parents="-p $head"
    for remote
    do
        parents="$parents -p $remote"
    done
    result_commit=$(echo "$merge_msg" | git-commit-tree $result_tree $parents) || exit
    finish "$result_commit" "Merge $result_commit, made by $wt_strategy."
    dropsave
    exit 0
fi

# Pick the result from the best strategy and have the user fix it up.
case "$best_strategy" in
'')
	restorestate
	echo >&2 "No merge strategy handled the merge."
	exit 2
	;;
"$wt_strategy")
	# We already have its result in the working tree.
	;;
*)
	echo "Rewinding the tree to pristine..."
	restorestate
	echo "Using the $best_strategy to prepare resolving by hand."
	git-merge-$best_strategy $common -- "$head_arg" "$@"
	;;
esac
for remote
do
	echo $remote
done >"$GIT_DIR/MERGE_HEAD"
echo $merge_msg >"$GIT_DIR/MERGE_MSG"

die "Automatic merge failed/prevented; fix up by hand"
