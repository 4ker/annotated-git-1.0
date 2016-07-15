#!/bin/sh
#
# Copyright (c) 2005 Johannes Schindelin
#

test_description='Test git-repo-config in different settings'

. ./test-lib.sh

test -f .git/config && rm .git/config

git-repo-config core.penguin "little blue"

cat > expect << EOF
[core]
	penguin = little blue
EOF

test_expect_success 'initial' 'cmp .git/config expect'

git-repo-config Core.Movie BadPhysics

cat > expect << EOF
[core]
	penguin = little blue
	Movie = BadPhysics
EOF

test_expect_success 'mixed case' 'cmp .git/config expect'

git-repo-config Cores.WhatEver Second

cat > expect << EOF
[core]
	penguin = little blue
	Movie = BadPhysics
[Cores]
	WhatEver = Second
EOF

test_expect_success 'similar section' 'cmp .git/config expect'

git-repo-config CORE.UPPERCASE true

cat > expect << EOF
[core]
	penguin = little blue
	Movie = BadPhysics
	UPPERCASE = true
[Cores]
	WhatEver = Second
EOF

test_expect_success 'similar section' 'cmp .git/config expect'

test_expect_success 'replace with non-match' \
	'git-repo-config core.penguin kingpin !blue'

test_expect_success 'replace with non-match (actually matching)' \
	'git-repo-config core.penguin "very blue" !kingpin'

cat > expect << EOF
[core]
	penguin = very blue
	Movie = BadPhysics
	UPPERCASE = true
	penguin = kingpin
[Cores]
	WhatEver = Second
EOF

test_expect_success 'non-match result' 'cmp .git/config expect'

cat > .git/config << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
		haha   ="beta" # last silly comment
haha = hello
	haha = bello
[nextSection] noNewline = ouch
EOF

cp .git/config .git/config2

test_expect_success 'multiple unset' \
	'git-repo-config --unset-all beta.haha'

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
[nextSection] noNewline = ouch
EOF

test_expect_success 'multiple unset is correct' 'cmp .git/config expect'

mv .git/config2 .git/config

test_expect_success '--replace-all' \
	'git-repo-config --replace-all beta.haha gamma'

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
	haha = gamma
[nextSection] noNewline = ouch
EOF

test_expect_success 'all replaced' 'cmp .git/config expect'

git-repo-config beta.haha alpha

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
	haha = alpha
[nextSection] noNewline = ouch
EOF

test_expect_success 'really mean test' 'cmp .git/config expect'

git-repo-config nextsection.nonewline wow

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
	haha = alpha
[nextSection]
	nonewline = wow
EOF

test_expect_success 'really really mean test' 'cmp .git/config expect'

test_expect_success 'get value' 'test alpha = $(git-repo-config beta.haha)'
git-repo-config --unset beta.haha

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
[nextSection]
	nonewline = wow
EOF

test_expect_success 'unset' 'cmp .git/config expect'

git-repo-config nextsection.NoNewLine "wow2 for me" "for me$"

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
[nextSection]
	nonewline = wow
	NoNewLine = wow2 for me
EOF

test_expect_success 'multivar' 'cmp .git/config expect'

test_expect_success 'non-match' \
	'git-repo-config --get nextsection.nonewline !for'

test_expect_success 'non-match value' \
	'test wow = $(git-repo-config --get nextsection.nonewline !for)'

test_expect_failure 'ambiguous get' \
	'git-repo-config --get nextsection.nonewline'

test_expect_success 'get multivar' \
	'git-repo-config --get-all nextsection.nonewline'

git-repo-config nextsection.nonewline "wow3" "wow$"

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
[nextSection]
	nonewline = wow3
	NoNewLine = wow2 for me
EOF

test_expect_success 'multivar replace' 'cmp .git/config expect'

test_expect_failure 'ambiguous value' 'git-repo-config nextsection.nonewline'

test_expect_failure 'ambiguous unset' \
	'git-repo-config --unset nextsection.nonewline'

test_expect_failure 'invalid unset' \
	'git-repo-config --unset somesection.nonewline'

git-repo-config --unset nextsection.nonewline "wow3$"

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
[nextSection]
	NoNewLine = wow2 for me
EOF

test_expect_success 'multivar unset' 'cmp .git/config expect'

test_expect_failure 'invalid key' 'git-repo-config inval.2key blabla'

test_expect_success 'correct key' 'git-repo-config 123456.a123 987'

test_expect_success 'hierarchical section' \
	'git-repo-config 1.2.3.alpha beta'

cat > expect << EOF
[beta] ; silly comment # another comment
noIndent= sillyValue ; 'nother silly comment

# empty line
		; comment
[nextSection]
	NoNewLine = wow2 for me
[123456]
	a123 = 987
[1.2.3]
	alpha = beta
EOF

test_expect_success 'hierarchical section value' 'cmp .git/config expect'

test_done

