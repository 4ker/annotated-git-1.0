#!/usr/bin/perl -w

use strict;
use Getopt::Std;
use File::Temp qw(tempdir);
use Data::Dumper;
use File::Basename qw(basename);

unless ($ENV{GIT_DIR} && -r $ENV{GIT_DIR}){
    die "GIT_DIR is not defined or is unreadable";
}

our ($opt_h, $opt_p, $opt_v, $opt_c );

getopts('hpvc');

$opt_h && usage();

die "Need at least one commit identifier!" unless @ARGV;

# setup a tempdir
our ($tmpdir, $tmpdirname) = tempdir('git-cvsapplycommit-XXXXXX',
				     TMPDIR => 1,
				     CLEANUP => 1);

print Dumper(@ARGV);
# resolve target commit
my $commit;
$commit = pop @ARGV;
$commit = `git-rev-parse --verify "$commit"^0`;
chomp $commit;
if ($?) {
    die "The commit reference $commit did not resolve!";
}

# resolve what parent we want
my $parent;
if (@ARGV) {
    $parent = pop @ARGV;
    $parent =  `git-rev-parse --verify "$parent"^0"`;
    chomp $parent;
    if ($?) {
	die "The parent reference did not resolve!";
    }
}

# find parents from the commit itself
my @commit  = `git-cat-file commit $commit`;
my @parents;
foreach my $p (@commit) {
    if ($p =~ m/^$/) { # end of commit headers, we're done
	last;
    }
    if ($p =~ m/^parent (\w{40})$/) { # found a parent
	push @parents, $1;
    }
}

if ($parent) {
    # double check that it's a valid parent
    foreach my $p (@parents) {
	my $found;
	if ($p eq $parent) {
	    $found = 1;
	    last;
	}; # found it
	die "Did not find $parent in the parents for this commit!";
    }
} else { # we don't have a parent from the cmdline...
    if (@parents == 1) { # it's safe to get it from the commit
	$parent = $parents[0];
    } else { # or perhaps not!
	die "This commit has more than one parent -- please name the parent you want to use explicitly";
    }
}

$opt_v && print "Applying to CVS commit $commit from parent $parent\n";

# grab the commit message
`git-cat-file commit $commit | sed -e '1,/^\$/d' > .msg`;
$? && die "Error extracting the commit message";

my (@afiles, @dfiles, @mfiles);
my @files = `git-diff-tree -r $parent $commit`;
print @files;
$? && die "Error in git-diff-tree";
foreach my $f (@files) {
    chomp $f;
    my @fields = split(m/\s+/, $f);
    if ($fields[4] eq 'A') {
	push @afiles, $fields[5];
    }
    if ($fields[4] eq 'M') {
	push @mfiles, $fields[5];
    }
    if ($fields[4] eq 'R') {
	push @dfiles, $fields[5];
    }
}
$opt_v && print "The commit affects:\n ";
$opt_v && print join ("\n ", @afiles,@mfiles,@dfiles) . "\n\n";
undef @files; # don't need it anymore

# check that the files are clean and up to date according to cvs
my $dirty;
foreach my $f (@afiles, @mfiles, @dfiles) {
    # TODO:we need to handle removed in cvs and/or new (from git) 
    my $status = `cvs -q status "$f" | grep '^File: '`;

    unless ($status =~ m/Status: Up-to-date$/) {
	$dirty = 1;
	warn "File $f not up to date in your CVS checkout!\n";
    }
}
if ($dirty) {
    die "Exiting: your CVS tree is not clean for this merge.";
}

###
### NOTE: if you are planning to die() past this point
###       you MUST call cleanupcvs(@files) before die()
###


print "'Patching' binary files\n";

my @bfiles = `git-diff-tree -p $parent $commit | grep '^Binary'`;
@bfiles = map { chomp } @bfiles;
foreach my $f (@bfiles) {
    # check that the file in cvs matches the "old" file
    # extract the file to $tmpdir and comparre with cmp
    my $tree = `git-rev-parse $parent^{tree} `;
    chomp $tree;
    my $blob = `git-ls-tree $tree "$f" | cut -f 1 | cut -d ' ' -f 3`;
    chomp $blob;
    `git-cat-file blob $blob > $tmpdir/blob`;
    `cmp -q $f $tmpdir/blob`;
    if ($?) {
	warn "Binary file $f in CVS does not match parent.\n";
	$dirty = 1;
	next;
    }

    # replace with the new file
     `git-cat-file blob $blob > $f`;

    # TODO: something smart with file modes

}
if ($dirty) {
    cleanupcvs(@files);
    die "Exiting: Binary files in CVS do not match parent";
}

## apply non-binary changes
my $fuzz = $opt_p ? 0 : 2;

print "Patching non-binary files\n";
print `(git-diff-tree -p $parent -p $commit | patch -p1 -F $fuzz ) 2>&1`;

my $dirtypatch = 0;
if (($? >> 8) == 2) {
    cleanupcvs(@files);
    die "Exiting: Patch reported serious trouble -- you will have to apply this patch manually";
} elsif (($? >> 8) == 1) { # some hunks failed to apply
    $dirtypatch = 1;
}

foreach my $f (@afiles) {
    `cvs add $f`;
    if ($?) {
	$dirty = 1;
	warn "Failed to cvs add $f -- you may need to do it manually";
    }
}

foreach my $f (@dfiles) {
    `cvs rm -f $f`;
    if ($?) {
	$dirty = 1;
	warn "Failed to cvs rm -f $f -- you may need to do it manually";
    }
}

print "Commit to CVS\n";
my $commitfiles = join(' ', @afiles, @mfiles, @dfiles);
my $cmd = "cvs commit -F .msg $commitfiles";

if ($dirtypatch) {
    print "NOTE: One or more hunks failed to apply cleanly.\n";
    print "Resolve the conflicts and then commit using:\n";
    print "\n    $cmd\n\n";
    exit(1);
}


if ($opt_c) {
    print "Autocommit\n  $cmd\n";
    print `cvs commit -F .msg $commitfiles 2>&1`;
    if ($?) {
	cleanupcvs(@files);
	die "Exiting: The commit did not succeed";
    }
    print "Committed successfully to CVS\n";
} else {
    print "Ready for you to commit, just run:\n\n   $cmd\n";
}
sub usage {
	print STDERR <<END;
Usage: GIT_DIR=/path/to/.git ${\basename $0} [-h] [-p] [-v] [-c] [ parent ] commit
END
	exit(1);
}

# ensure cvs is clean before we die
sub cleanupcvs {
    my @files = @_;
    foreach my $f (@files) {
	`cvs -q update -C "$f"`;
	if ($?) {
	    warn "Warning! Failed to cleanup state of $f\n";
	}
    }
}

