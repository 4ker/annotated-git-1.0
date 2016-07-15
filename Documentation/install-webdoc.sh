#!/bin/sh

T="$1"

for h in *.html *.txt howto/*.txt howto/*.html
do
	diff -u -I'Last updated [0-9][0-9]-[A-Z][a-z][a-z]-' "$T/$h" "$h" || {
		echo >&2 "# install $h $T/$h"
		rm -f "$T/$h"
		mkdir -p `dirname "$T/$h"`
		cp "$h" "$T/$h"
	}
done
strip_leading=`echo "$T/" | sed -e 's|.|.|g'`
for th in "$T"/*.html "$T"/*.txt "$T"/howto/*.txt "$T"/howto/*.html
do
	h=`expr "$th" : "$strip_leading"'\(.*\)'`
	case "$h" in
	index.html) continue ;;
	esac
	test -f "$h" && continue
	echo >&2 "# rm -f $th"
	rm -f "$th"
done
ln -sf git.html "$T/index.html"
