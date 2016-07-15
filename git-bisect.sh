#!/bin/sh

USAGE='[start|bad|good|next|reset|visualize]'
LONG_USAGE='git bisect start [<pathspec>]	reset bisect state and start bisection.
git bisect bad [<rev>]		mark <rev> a known-bad revision.
git bisect good [<rev>...]	mark <rev>... known-good revisions.
git bisect next			find next bisection to test and check it out.
git bisect reset [<branch>]	finish bisection search and go back to branch.
git bisect visualize            show bisect status in gitk.
git bisect replay <logfile>	replay bisection log
git bisect log			show bisect log.'

. git-sh-setup

sq() {
	perl -e '
		for (@ARGV) {
			s/'\''/'\'\\\\\'\''/g;
			print " '\''$_'\''";
		}
		print "\n";
	' "$@"
}

bisect_autostart() {
	test -d "$GIT_DIR/refs/bisect" || {
		echo >&2 'You need to start by "git bisect start"'
		if test -t 0
		then
			echo >&2 -n 'Do you want me to do it for you [Y/n]? '
			read yesno
			case "$yesno" in
			[Nn]*)
				exit ;;
			esac
			bisect_start
		else
			exit 1
		fi
	}
}

bisect_start() {
	#
	# Verify HEAD. If we were bisecting before this, reset to the
	# top-of-line master first!
	#
	head=$(GIT_DIR="$GIT_DIR" git-symbolic-ref HEAD) ||
	die "Bad HEAD - I need a symbolic ref"
	case "$head" in
	refs/heads/bisect*)
		git checkout master || exit
		;;
	refs/heads/*)
		;;
	*)
		die "Bad HEAD - strange symbolic ref"
		;;
	esac

	#
	# Get rid of any old bisect state
	#
	rm -f "$GIT_DIR/refs/heads/bisect"
	rm -rf "$GIT_DIR/refs/bisect/"
	mkdir "$GIT_DIR/refs/bisect"
	{
	    printf "git-bisect start"
	    sq "$@"
	} >"$GIT_DIR/BISECT_LOG"
	sq "$@" >"$GIT_DIR/BISECT_NAMES"
}

bisect_bad() {
	bisect_autostart
	case "$#" in
	0)
		rev=$(git-rev-parse --verify HEAD) ;;
	1)
		rev=$(git-rev-parse --verify "$1") ;;
	*)
		usage ;;
	esac || exit
	echo "$rev" >"$GIT_DIR/refs/bisect/bad"
	echo "# bad: "$(git-show-branch $rev) >>"$GIT_DIR/BISECT_LOG"
	echo "git-bisect bad $rev" >>"$GIT_DIR/BISECT_LOG"
	bisect_auto_next
}

bisect_good() {
	bisect_autostart
        case "$#" in
	0)    revs=$(git-rev-parse --verify HEAD) || exit ;;
	*)    revs=$(git-rev-parse --revs-only --no-flags "$@") &&
		test '' != "$revs" || die "Bad rev input: $@" ;;
	esac
	for rev in $revs
	do
		rev=$(git-rev-parse --verify "$rev") || exit
		echo "$rev" >"$GIT_DIR/refs/bisect/good-$rev"
		echo "# good: "$(git-show-branch $rev) >>"$GIT_DIR/BISECT_LOG"
		echo "git-bisect good $rev" >>"$GIT_DIR/BISECT_LOG"
	done
	bisect_auto_next
}

bisect_next_check() {
	next_ok=no
        test -f "$GIT_DIR/refs/bisect/bad" &&
	case "$(cd "$GIT_DIR" && echo refs/bisect/good-*)" in
	refs/bisect/good-\*) ;;
	*) next_ok=yes ;;
	esac
	case "$next_ok,$1" in
	no,) false ;;
	no,fail)
	    echo >&2 'You need to give me at least one good and one bad revisions.'
	    exit 1 ;;
	*)
	    true ;;
	esac
}

bisect_auto_next() {
	bisect_next_check && bisect_next || :
}

bisect_next() {
        case "$#" in 0) ;; *) usage ;; esac
	bisect_autostart
	bisect_next_check fail
	bad=$(git-rev-parse --verify refs/bisect/bad) &&
	good=$(git-rev-parse --sq --revs-only --not \
		$(cd "$GIT_DIR" && ls refs/bisect/good-*)) &&
	rev=$(eval "git-rev-list --bisect $good $bad -- $(cat $GIT_DIR/BISECT_NAMES)") || exit
	if [ -z "$rev" ]; then
	    echo "$bad was both good and bad"
	    exit 1
	fi
	if [ "$rev" = "$bad" ]; then
	    echo "$rev is first bad commit"
	    git-diff-tree --pretty $rev
	    exit 0
	fi
	nr=$(eval "git-rev-list $rev $good -- $(cat $GIT_DIR/BISECT_NAMES)" | wc -l) || exit
	echo "Bisecting: $nr revisions left to test after this"
	echo "$rev" > "$GIT_DIR/refs/heads/new-bisect"
	git checkout new-bisect || exit
	mv "$GIT_DIR/refs/heads/new-bisect" "$GIT_DIR/refs/heads/bisect" &&
	GIT_DIR="$GIT_DIR" git-symbolic-ref HEAD refs/heads/bisect
	git-show-branch "$rev"
}

bisect_visualize() {
	bisect_next_check fail
	not=`cd "$GIT_DIR/refs" && echo bisect/good-*`
	eval gitk bisect/bad --not $not -- $(cat "$GIT_DIR/BISECT_NAMES")
}

bisect_reset() {
	case "$#" in
	0) branch=master ;;
	1) test -f "$GIT_DIR/refs/heads/$1" || {
	       echo >&2 "$1 does not seem to be a valid branch"
	       exit 1
	   }
	   branch="$1" ;;
        *)
	    usage ;;
	esac
	git checkout "$branch" &&
	rm -fr "$GIT_DIR/refs/bisect"
	rm -f "$GIT_DIR/refs/heads/bisect"
	rm -f "$GIT_DIR/BISECT_LOG"
}

bisect_replay () {
	test -r "$1" || {
		echo >&2 "cannot read $1 for replaying"
		exit 1
	}
	bisect_reset
	while read bisect command rev
	do
		test "$bisect" = "git-bisect" || continue
		case "$command" in
		start)
			cmd="bisect_start $rev"
			eval "$cmd"
			;;
		good)
			echo "$rev" >"$GIT_DIR/refs/bisect/good-$rev"
			echo "# good: "$(git-show-branch $rev) >>"$GIT_DIR/BISECT_LOG"
			echo "git-bisect good $rev" >>"$GIT_DIR/BISECT_LOG"
			;;
		bad)
			echo "$rev" >"$GIT_DIR/refs/bisect/bad"
			echo "# bad: "$(git-show-branch $rev) >>"$GIT_DIR/BISECT_LOG"
			echo "git-bisect bad $rev" >>"$GIT_DIR/BISECT_LOG"
			;;
		*)
			echo >&2 "?? what are you talking about?"
			exit 1 ;;
		esac
	done <"$1"
	bisect_auto_next
}

case "$#" in
0)
    usage ;;
*)
    cmd="$1"
    shift
    case "$cmd" in
    start)
        bisect_start "$@" ;;
    bad)
        bisect_bad "$@" ;;
    good)
        bisect_good "$@" ;;
    next)
        # Not sure we want "next" at the UI level anymore.
        bisect_next "$@" ;;
    visualize)
	bisect_visualize "$@" ;;
    reset)
        bisect_reset "$@" ;;
    replay)
	bisect_replay "$@" ;;
    log)
	cat "$GIT_DIR/BISECT_LOG" ;;
    *)
        usage ;;
    esac
esac
