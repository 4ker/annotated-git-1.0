#!/bin/sh

# git-ls-remote could be called from outside a git managed repository;
# this would fail in that case and would issue an error message.
GIT_DIR=$(git-rev-parse --git-dir 2>/dev/null) || :;

get_data_source () {
	case "$1" in
	*/*)
		# Not so fast.	This could be the partial URL shorthand...
		token=$(expr "$1" : '\([^/]*\)/')
		remainder=$(expr "$1" : '[^/]*/\(.*\)')
		if test -f "$GIT_DIR/branches/$token"
		then
			echo branches-partial
		else
			echo ''
		fi
		;;
	*)
		if test -f "$GIT_DIR/remotes/$1"
		then
			echo remotes
		elif test -f "$GIT_DIR/branches/$1"
		then
			echo branches
		else
			echo ''
		fi ;;
	esac
}

get_remote_url () {
	data_source=$(get_data_source "$1")
	case "$data_source" in
	'')
		echo "$1" ;;
	remotes)
		sed -ne '/^URL: */{
			s///p
			q
		}' "$GIT_DIR/remotes/$1" ;;
	branches)
		sed -e 's/#.*//' "$GIT_DIR/branches/$1" ;;
	branches-partial)
		token=$(expr "$1" : '\([^/]*\)/')
		remainder=$(expr "$1" : '[^/]*/\(.*\)')
		url=$(sed -e 's/#.*//' "$GIT_DIR/branches/$token")
		echo "$url/$remainder"
		;;
	*)
		die "internal error: get-remote-url $1" ;;
	esac
}

get_remote_default_refs_for_push () {
	data_source=$(get_data_source "$1")
	case "$data_source" in
	'' | branches | branches-partial)
		;; # no default push mapping, just send matching refs.
	remotes)
		sed -ne '/^Push: */{
			s///p
		}' "$GIT_DIR/remotes/$1" ;;
	*)
		die "internal error: get-remote-default-ref-for-push $1" ;;
	esac
}

# Subroutine to canonicalize remote:local notation.
canon_refs_list_for_fetch () {
	# Leave only the first one alone; add prefix . to the rest
	# to prevent the secondary branches to be merged by default.
	dot_prefix=
	for ref
	do
		force=
		case "$ref" in
		+*)
			ref=$(expr "$ref" : '\+\(.*\)')
			force=+
			;;
		esac
		expr "$ref" : '.*:' >/dev/null || ref="${ref}:"
		remote=$(expr "$ref" : '\([^:]*\):')
		local=$(expr "$ref" : '[^:]*:\(.*\)')
		case "$remote" in
		'') remote=HEAD ;;
		refs/heads/* | refs/tags/*) ;;
		heads/* | tags/* ) remote="refs/$remote" ;;
		*) remote="refs/heads/$remote" ;;
		esac
		case "$local" in
		'') local= ;;
		refs/heads/* | refs/tags/*) ;;
		heads/* | tags/* ) local="refs/$local" ;;
		*) local="refs/heads/$local" ;;
		esac

		if local_ref_name=$(expr "$local" : 'refs/\(.*\)')
		then
		   git-check-ref-format "$local_ref_name" ||
		   die "* refusing to create funny ref '$local_ref_name' locally"
		fi
		echo "${dot_prefix}${force}${remote}:${local}"
		dot_prefix=.
	done
}

# Returns list of src: (no store), or src:dst (store)
get_remote_default_refs_for_fetch () {
	data_source=$(get_data_source "$1")
	case "$data_source" in
	'' | branches-partial)
		echo "HEAD:" ;;
	branches)
		remote_branch=$(sed -ne '/#/s/.*#//p' "$GIT_DIR/branches/$1")
		case "$remote_branch" in '') remote_branch=master ;; esac
		echo "refs/heads/${remote_branch}:refs/heads/$1"
		;;
	remotes)
		# This prefixes the second and later default refspecs
		# with a '.', to signal git-fetch to mark them
		# not-for-merge.
		canon_refs_list_for_fetch $(sed -ne '/^Pull: */{
						s///p
					}' "$GIT_DIR/remotes/$1")
		;;
	*)
		die "internal error: get-remote-default-ref-for-push $1" ;;
	esac
}

get_remote_refs_for_push () {
	case "$#" in
	0) die "internal error: get-remote-refs-for-push." ;;
	1) get_remote_default_refs_for_push "$@" ;;
	*) shift; echo "$@" ;;
	esac
}

get_remote_refs_for_fetch () {
	case "$#" in
	0)
	    die "internal error: get-remote-refs-for-fetch." ;;
	1)
	    get_remote_default_refs_for_fetch "$@" ;;
	*)
	    shift
	    tag_just_seen=
	    for ref
	    do
		if test "$tag_just_seen"
		then
		    echo "refs/tags/${ref}:refs/tags/${ref}"
		    tag_just_seen=
		    continue
		else
		    case "$ref" in
		    tag)
			tag_just_seen=yes
			continue
			;;
		    esac
		fi
		canon_refs_list_for_fetch "$ref"
	    done
	    ;;
	esac
}

resolve_alternates () {
	# original URL (xxx.git)
	top_=`expr "$1" : '\([^:]*:/*[^/]*\)/'`
	while read path
	do
		case "$path" in
		\#* | '')
			continue ;;
		/*)
			echo "$top_$path/" ;;
		../*)
			# relative -- ugly but seems to work.
			echo "$1/objects/$path/" ;;
		*)
			# exit code may not be caught by the reader.
			echo "bad alternate: $path"
			exit 1 ;;
		esac
	done
}
