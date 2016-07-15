#!/bin/sh
#
# Copyright (c) 2005 Linus Torvalds
# Copyright (c) 2005 Junio C Hamano

USAGE='[ --diff-options ] <ent>{0,2} [<path>...]'
SUBDIRECTORY_OK='Yes'
. git-sh-setup

rev=$(git-rev-parse --revs-only --no-flags --sq "$@") || exit
flags=$(git-rev-parse --no-revs --flags --sq "$@")
files=$(git-rev-parse --no-revs --no-flags --sq "$@")

# I often say 'git diff --cached -p' and get scolded by git-diff-files, but
# obviously I mean 'git diff --cached -p HEAD' in that case.
case "$rev" in
'')
	case " $flags " in
	*" '--cached' "*)
		rev='HEAD '
		;;
	esac
esac

# If we do not have --name-status, --name-only nor -r, default to -p.
# If we do not have -B nor -C, default to -M.
case " $flags " in
*" '--name-status' "* | *" '--name-only' "* | *" '-r' "* )
	;;
*)
	flags="$flags'-p' " ;;
esac
case " $flags " in
*" '-"[BCM]* | *" '--find-copies-harder' "*)
	;; # something like -M50.
*)
	flags="$flags'-M' " ;;
esac

case "$rev" in
?*' '?*' '?*)
	usage
	;;
?*' '^?*)
	begin=$(expr "$rev" : '.*^.\([0-9a-f]*\).*') &&
	end=$(expr "$rev" : '.\([0-9a-f]*\). .*') || exit
	cmd="git-diff-tree $flags $begin $end -- $files"
	;;
?*' '?*)
	cmd="git-diff-tree $flags $rev -- $files"
	;;
?*' ')
	cmd="git-diff-index $flags $rev -- $files"
	;;
'')
	cmd="git-diff-files $flags -- $files"
	;;
*)
	usage
	;;
esac

eval "$cmd"
