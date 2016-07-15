#!/bin/sh
#
#

USAGE='[--signoff] [--dotest=<dir>] [--utf8] [--binary] [--3way] <mbox>
  or, when resuming [--skip | --resolved]'
. git-sh-setup

stop_here () {
    echo "$1" >"$dotest/next"
    exit 1
}

go_next () {
	rm -f "$dotest/$msgnum" "$dotest/msg" "$dotest/msg-clean" \
		"$dotest/patch" "$dotest/info"
	echo "$next" >"$dotest/next"
	this=$next
}

fall_back_3way () {
    O_OBJECT=`cd "$GIT_OBJECT_DIRECTORY" && pwd`

    rm -fr "$dotest"/patch-merge-*
    mkdir "$dotest/patch-merge-tmp-dir"

    # First see if the patch records the index info that we can use.
    if git-apply -z --index-info "$dotest/patch" \
	>"$dotest/patch-merge-index-info" 2>/dev/null &&
	GIT_INDEX_FILE="$dotest/patch-merge-tmp-index" \
	git-update-index -z --index-info <"$dotest/patch-merge-index-info" &&
	GIT_INDEX_FILE="$dotest/patch-merge-tmp-index" \
	git-write-tree >"$dotest/patch-merge-base+" &&
	# index has the base tree now.
	(
	    cd "$dotest/patch-merge-tmp-dir" &&
	    GIT_INDEX_FILE="../patch-merge-tmp-index" \
	    GIT_OBJECT_DIRECTORY="$O_OBJECT" \
	    git-apply $binary --index <../patch
        )
    then
	echo Using index info to reconstruct a base tree...
	mv "$dotest/patch-merge-base+" "$dotest/patch-merge-base"
	mv "$dotest/patch-merge-tmp-index" "$dotest/patch-merge-index"
    else
	# Otherwise, try nearby trees that can be used to apply the
	# patch.
	(
	    N=10

	    # Hoping the patch is against our recent commits...
	    git-rev-list --max-count=$N HEAD

	    # or hoping the patch is against known tags...
	    git-ls-remote --tags .
	) |
	while read base junk
	do
	    # See if we have it as a tree...
	    git-cat-file tree "$base" >/dev/null 2>&1 || continue

	    rm -fr "$dotest"/patch-merge-* &&
	    mkdir "$dotest/patch-merge-tmp-dir" || break
	    (
		cd "$dotest/patch-merge-tmp-dir" &&
		GIT_INDEX_FILE=../patch-merge-tmp-index &&
		GIT_OBJECT_DIRECTORY="$O_OBJECT" &&
		export GIT_INDEX_FILE GIT_OBJECT_DIRECTORY &&
		git-read-tree "$base" &&
		git-apply $binary --index &&
		mv ../patch-merge-tmp-index ../patch-merge-index &&
		echo "$base" >../patch-merge-base
	    ) <"$dotest/patch"  2>/dev/null && break
	done
    fi

    test -f "$dotest/patch-merge-index" &&
    his_tree=$(GIT_INDEX_FILE="$dotest/patch-merge-index" git-write-tree) &&
    orig_tree=$(cat "$dotest/patch-merge-base") &&
    rm -fr "$dotest"/patch-merge-* || exit 1

    echo Falling back to patching base and 3-way merge...

    # This is not so wrong.  Depending on which base we picked,
    # orig_tree may be wildly different from ours, but his_tree
    # has the same set of wildly different changes in parts the
    # patch did not touch, so resolve ends up cancelling them,
    # saying that we reverted all those changes.

    git-merge-resolve $orig_tree -- HEAD $his_tree || {
	    echo Failed to merge in the changes.
	    exit 1
    }
}

prec=4
dotest=.dotest sign= utf8= keep= skip= interactive= resolved= binary=

while case "$#" in 0) break;; esac
do
	case "$1" in
	-d=*|--d=*|--do=*|--dot=*|--dote=*|--dotes=*|--dotest=*)
	dotest=`expr "$1" : '-[^=]*=\(.*\)'`; shift ;;
	-d|--d|--do|--dot|--dote|--dotes|--dotest)
	case "$#" in 1) usage ;; esac; shift
	dotest="$1"; shift;;

	-i|--i|--in|--int|--inte|--inter|--intera|--interac|--interact|\
	--interacti|--interactiv|--interactive)
	interactive=t; shift ;;

	-b|--b|--bi|--bin|--bina|--binar|--binary)
	binary=t; shift ;;

	-3|--3|--3w|--3wa|--3way)
	threeway=t; shift ;;
	-s|--s|--si|--sig|--sign|--signo|--signof|--signoff)
	sign=t; shift ;;
	-u|--u|--ut|--utf|--utf8)
	utf8=t; shift ;;
	-k|--k|--ke|--kee|--keep)
	keep=t; shift ;;

	-r|--r|--re|--res|--reso|--resol|--resolv|--resolve|--resolved)
	resolved=t; shift ;;

	--sk|--ski|--skip)
	skip=t; shift ;;

	--)
	shift; break ;;
	-*)
	usage ;;
	*)
	break ;;
	esac
done

# If the dotest directory exists, but we have finished applying all the
# patches in them, clear it out.
if test -d "$dotest" &&
   last=$(cat "$dotest/last") &&
   next=$(cat "$dotest/next") &&
   test $# != 0 &&
   test "$next" -gt "$last"
then
   rm -fr "$dotest"
fi

if test -d "$dotest"
then
	test ",$#," = ",0," ||
	die "previous dotest directory $dotest still exists but mbox given."
	resume=yes
else
	# Make sure we are not given --skip nor --resolved
	test ",$skip,$resolved," = ,,, ||
		die "we are not resuming."

	# Start afresh.
	mkdir -p "$dotest" || exit

	git-mailsplit -d"$prec" -o"$dotest" -b -- "$@" > "$dotest/last" ||  {
		rm -fr "$dotest"
		exit 1
	}

	# -b, -s, -u and -k flags are kept for the resuming session after
	# a patch failure.
	# -3 and -i can and must be given when resuming.
	echo "$binary" >"$dotest/binary"
	echo "$sign" >"$dotest/sign"
	echo "$utf8" >"$dotest/utf8"
	echo "$keep" >"$dotest/keep"
	echo 1 >"$dotest/next"
fi

case "$resolved" in
'')
	files=$(git-diff-index --cached --name-only HEAD) || exit
	if [ "$files" ]; then
	   echo "Dirty index: cannot apply patches (dirty: $files)" >&2
	   exit 1
	fi
esac

if test "$(cat "$dotest/binary")" = t
then
	binary=--allow-binary-replacement
fi
if test "$(cat "$dotest/utf8")" = t
then
	utf8=-u
fi
if test "$(cat "$dotest/keep")" = t
then
	keep=-k
fi
if test "$(cat "$dotest/sign")" = t
then
	SIGNOFF=`git-var GIT_COMMITTER_IDENT | sed -e '
			s/>.*/>/
			s/^/Signed-off-by: /'
		`
else
	SIGNOFF=
fi

last=`cat "$dotest/last"`
this=`cat "$dotest/next"`
if test "$skip" = t
then
	this=`expr "$this" + 1`
	resume=
fi

if test "$this" -gt "$last"
then
	echo Nothing to do.
	rm -fr "$dotest"
	exit
fi

while test "$this" -le "$last"
do
	msgnum=`printf "%0${prec}d" $this`
	next=`expr "$this" + 1`
	test -f "$dotest/$msgnum" || {
		resume=
		go_next
		continue
	}

	# If we are not resuming, parse and extract the patch information
	# into separate files:
	#  - info records the authorship and title
	#  - msg is the rest of commit log message
	#  - patch is the patch body.
	#
	# When we are resuming, these files are either already prepared
	# by the user, or the user can tell us to do so by --resolved flag.
	case "$resume" in
	'')
		git-mailinfo $keep $utf8 "$dotest/msg" "$dotest/patch" \
			<"$dotest/$msgnum" >"$dotest/info" ||
			stop_here $this
		git-stripspace < "$dotest/msg" > "$dotest/msg-clean"
		;;
	esac

	GIT_AUTHOR_NAME="$(sed -n '/^Author/ s/Author: //p' "$dotest/info")"
	GIT_AUTHOR_EMAIL="$(sed -n '/^Email/ s/Email: //p' "$dotest/info")"
	GIT_AUTHOR_DATE="$(sed -n '/^Date/ s/Date: //p' "$dotest/info")"

	if test -z "$GIT_AUTHOR_EMAIL"
	then
		echo "Patch does not have a valid e-mail address."
		stop_here $this
	fi

	export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_AUTHOR_DATE

	SUBJECT="$(sed -n '/^Subject/ s/Subject: //p' "$dotest/info")"
	case "$keep_subject" in -k)  SUBJECT="[PATCH] $SUBJECT" ;; esac

	case "$resume" in
	'')
	    if test '' != "$SIGNOFF"
	    then
		LAST_SIGNED_OFF_BY=`
		    sed -ne '/^Signed-off-by: /p' \
		    "$dotest/msg-clean" |
		    tail -n 1
		`
		ADD_SIGNOFF=`
		    test "$LAST_SIGNED_OFF_BY" = "$SIGNOFF" || {
		    test '' = "$LAST_SIGNED_OFF_BY" && echo
		    echo "$SIGNOFF"
		}`
	    else
		ADD_SIGNOFF=
	    fi
	    {
		echo "$SUBJECT"
		if test -s "$dotest/msg-clean"
		then
			echo
			cat "$dotest/msg-clean"
		fi
		if test '' != "$ADD_SIGNOFF"
		then
			echo "$ADD_SIGNOFF"
		fi
	    } >"$dotest/final-commit"
	    ;;
	*)
		case "$resolved,$interactive" in
		tt)
			# This is used only for interactive view option.
			git-diff-index -p --cached HEAD >"$dotest/patch"
			;;
		esac
	esac

	resume=
	if test "$interactive" = t
	then
	    test -t 0 ||
	    die "cannot be interactive without stdin connected to a terminal."
	    action=again
	    while test "$action" = again
	    do
		echo "Commit Body is:"
		echo "--------------------------"
		cat "$dotest/final-commit"
		echo "--------------------------"
		printf "Apply? [y]es/[n]o/[e]dit/[v]iew patch/[a]ccept all "
		read reply
		case "$reply" in
		[yY]*) action=yes ;;
		[aA]*) action=yes interactive= ;;
		[nN]*) action=skip ;;
		[eE]*) "${VISUAL:-${EDITOR:-vi}}" "$dotest/final-commit"
		       action=again ;;
		[vV]*) action=again
		       LESS=-S ${PAGER:-less} "$dotest/patch" ;;
		*)     action=again ;;
		esac
	    done
	else
	    action=yes
	fi

	if test $action = skip
	then
		go_next
		continue
	fi

	if test -x "$GIT_DIR"/hooks/applypatch-msg
	then
		"$GIT_DIR"/hooks/applypatch-msg "$dotest/final-commit" ||
		stop_here $this
	fi

	echo
	echo "Applying '$SUBJECT'"
	echo

	case "$resolved" in
	'')
		git-apply $binary --index "$dotest/patch"
		apply_status=$?
		;;
	t)
		# Resolved means the user did all the hard work, and
		# we do not have to do any patch application.  Just
		# trust what the user has in the index file and the
		# working tree.
		resolved=
		apply_status=0
		;;
	esac

	if test $apply_status = 1 && test "$threeway" = t
	then
		if (fall_back_3way)
		then
		    # Applying the patch to an earlier tree and merging the
		    # result may have produced the same tree as ours.
		    changed="$(git-diff-index --cached --name-only -z HEAD)"
		    if test '' = "$changed"
		    then
			    echo No changes -- Patch already applied.
			    go_next
			    continue
		    fi
		    # clear apply_status -- we have successfully merged.
		    apply_status=0
		fi
	fi
	if test $apply_status != 0
	then
		echo Patch failed at $msgnum.
		stop_here $this
	fi

	if test -x "$GIT_DIR"/hooks/pre-applypatch
	then
		"$GIT_DIR"/hooks/pre-applypatch || stop_here $this
	fi

	tree=$(git-write-tree) &&
	echo Wrote tree $tree &&
	parent=$(git-rev-parse --verify HEAD) &&
	commit=$(git-commit-tree $tree -p $parent <"$dotest/final-commit") &&
	echo Committed: $commit &&
	git-update-ref HEAD $commit $parent ||
	stop_here $this

	if test -x "$GIT_DIR"/hooks/post-applypatch
	then
		"$GIT_DIR"/hooks/post-applypatch
	fi

	go_next
done

rm -fr "$dotest"
