#!/usr/bin/perl
#
# Copyright 2005, Ryan Anderson <ryan@michonline.com>
#                 Josef Weidendorfer <Josef.Weidendorfer@gmx.de>
#
# This file is licensed under the GPL v2, or a later version
# at the discretion of Linus Torvalds.


use warnings;
use strict;
use Getopt::Std;

sub usage() {
	print <<EOT;
$0 [-f] [-n] <source> <destination>
$0 [-f] [-n] [-k] <source> ... <destination directory>
EOT
	exit(1);
}

my $GIT_DIR = `git rev-parse --git-dir`;
exit 1 if $?; # rev-parse would have given "not a git dir" message.
chomp($GIT_DIR);

our ($opt_n, $opt_f, $opt_h, $opt_k, $opt_v);
getopts("hnfkv") || usage;
usage() if $opt_h;
@ARGV >= 1 or usage;

my (@srcArgs, @dstArgs, @srcs, @dsts);
my ($src, $dst, $base, $dstDir);

my $argCount = scalar @ARGV;
if (-d $ARGV[$argCount-1]) {
	$dstDir = $ARGV[$argCount-1];
	# remove any trailing slash
	$dstDir =~ s/\/$//;
	@srcArgs = @ARGV[0..$argCount-2];
	
	foreach $src (@srcArgs) {
		$base = $src;
		$base =~ s/^.*\///;
		$dst = "$dstDir/". $base;
		push @dstArgs, $dst;
	}
}
else {
    if ($argCount != 2) {
	print "Error: moving to directory '"
	    . $ARGV[$argCount-1]
	    . "' not possible; not exisiting\n";
	exit(1);
    }
    @srcArgs = ($ARGV[0]);
    @dstArgs = ($ARGV[1]);
    $dstDir = "";
}

my (@allfiles,@srcfiles,@dstfiles);
my $safesrc;
my (%overwritten, %srcForDst);

$/ = "\0";
open(F, 'git-ls-files -z |')
        or die "Failed to open pipe from git-ls-files: " . $!;

@allfiles = map { chomp; $_; } <F>;
close(F);


my ($i, $bad);
while(scalar @srcArgs > 0) {
    $src = shift @srcArgs;
    $dst = shift @dstArgs;
    $bad = "";

    if ($opt_v) {
	print "Checking rename of '$src' to '$dst'\n";
    }

    unless (-f $src || -l $src || -d $src) {
	$bad = "bad source '$src'";
    }

    $safesrc = quotemeta($src);
    @srcfiles = grep /^$safesrc(\/|$)/, @allfiles;

    $overwritten{$dst} = 0;
    if (($bad eq "") && -e $dst) {
	$bad = "destination '$dst' already exists";
	if ($opt_f) {
	    # only files can overwrite each other: check both source and destination
	    if (-f $dst && (scalar @srcfiles == 1)) {
		print "Warning: $bad; will overwrite!\n";
		$bad = "";
		$overwritten{$dst} = 1;
	    }
	    else {
		$bad = "Can not overwrite '$src' with '$dst'";
	    }
	}
    }
    
    if (($bad eq "") && ($dst =~ /^$safesrc\//)) {
	$bad = "can not move directory '$src' into itself";
    }

    if ($bad eq "") {
        if (scalar @srcfiles == 0) {
	    $bad = "'$src' not under version control";
	}
    }

    if ($bad eq "") {
       if (defined $srcForDst{$dst}) {
           $bad = "can not move '$src' to '$dst'; already target of ";
           $bad .= "'".$srcForDst{$dst}."'";
       }
       else {
           $srcForDst{$dst} = $src;
       }
    }

    if ($bad ne "") {
	if ($opt_k) {
	    print "Warning: $bad; skipping\n";
	    next;
	}
	print "Error: $bad\n";
	exit(1);
    }
    push @srcs, $src;
    push @dsts, $dst;
}

# Final pass: rename/move
my (@deletedfiles,@addedfiles,@changedfiles);
$bad = "";
while(scalar @srcs > 0) {
    $src = shift @srcs;
    $dst = shift @dsts;

    if ($opt_n || $opt_v) { print "Renaming $src to $dst\n"; }
    if (!$opt_n) {
	if (!rename($src,$dst)) {
	    $bad = "renaming '$src' failed: $!";
	    if ($opt_k) {
		print "Warning: skipped: $bad\n";
		$bad = "";
		next;
	    }
	    last;
	}
    }

    $safesrc = quotemeta($src);
    @srcfiles = grep /^$safesrc(\/|$)/, @allfiles;
    @dstfiles = @srcfiles;
    s/^$safesrc(\/|$)/$dst$1/ for @dstfiles;

    push @deletedfiles, @srcfiles;
    if (scalar @srcfiles == 1) {
	# $dst can be a directory with 1 file inside
	if ($overwritten{$dst} ==1) {
	    push @changedfiles, $dstfiles[0];

	} else {
	    push @addedfiles, $dstfiles[0];
	}
    }
    else {
	push @addedfiles, @dstfiles;
    }
}

if ($opt_n) {
    if (@changedfiles) {
	print "Changed  : ". join(", ", @changedfiles) ."\n";
    }
    if (@addedfiles) {
	print "Adding   : ". join(", ", @addedfiles) ."\n";
    }
    if (@deletedfiles) {
	print "Deleting : ". join(", ", @deletedfiles) ."\n";
    }
}
else {
    if (@changedfiles) {
	open(H, "| git-update-index -z --stdin")
		or die "git-update-index failed to update changed files with code $!\n";
	foreach my $fileName (@changedfiles) {
		print H "$fileName\0";
	}
	close(H);
    }
    if (@addedfiles) {
	open(H, "| git-update-index --add -z --stdin")
		or die "git-update-index failed to add new names with code $!\n";
	foreach my $fileName (@addedfiles) {
		print H "$fileName\0";
	}
	close(H);
    }
    if (@deletedfiles) {
	open(H, "| git-update-index --remove -z --stdin")
		or die "git-update-index failed to remove old names with code $!\n";
	foreach my $fileName (@deletedfiles) {
		print H "$fileName\0";
	}
	close(H);
    }
}

if ($bad ne "") {
    print "Error: $bad\n";
    exit(1);
}
