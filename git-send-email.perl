#!/usr/bin/perl -w
#
# Copyright 2002,2005 Greg Kroah-Hartman <greg@kroah.com>
# Copyright 2005 Ryan Anderson <ryan@michonline.com>
#
# GPL v2 (See COPYING)
#
# Ported to support git "mbox" format files by Ryan Anderson <ryan@michonline.com>
#
# Sends a collection of emails to the given email addresses, disturbingly fast.
#
# Supports two formats:
# 1. mbox format files (ignoring most headers and MIME formatting - this is designed for sending patches)
# 2. The original format support by Greg's script:
#    first line of the message is who to CC,
#    and second line is the subject of the message.
#

use strict;
use warnings;
use Term::ReadLine;
use Mail::Sendmail qw(sendmail %mailcfg);
use Getopt::Long;
use Data::Dumper;
use Email::Valid;

sub unique_email_list(@);
sub cleanup_compose_files();

# Constants (essentially)
my $compose_filename = ".msg.$$";

# Variables we fill in automatically, or via prompting:
my (@to,@cc,$initial_reply_to,$initial_subject,@files,$from,$compose);

# Behavior modification variables
my ($chain_reply_to, $smtp_server) = (1, "localhost");

# Example reply to:
#$initial_reply_to = ''; #<20050203173208.GA23964@foobar.com>';

my $term = new Term::ReadLine 'git-send-email';

# Begin by accumulating all the variables (defined above), that we will end up
# needing, first, from the command line:

my $rc = GetOptions("from=s" => \$from,
                    "in-reply-to=s" => \$initial_reply_to,
		    "subject=s" => \$initial_subject,
		    "to=s" => \@to,
		    "chain-reply-to!" => \$chain_reply_to,
		    "smtp-server=s" => \$smtp_server,
		    "compose" => \$compose,
	 );

# Now, let's fill any that aren't set in with defaults:

open(GITVAR,"-|","git-var","-l")
	or die "Failed to open pipe from git-var: $!";

my ($author,$committer);
while(<GITVAR>) {
	chomp;
	my ($var,$data) = split /=/,$_,2;
	my @fields = split /\s+/, $data;

	my $ident = join(" ", @fields[0...(@fields-3)]);

	if ($var eq 'GIT_AUTHOR_IDENT') {
		$author = $ident;
	} elsif ($var eq 'GIT_COMMITTER_IDENT') {
		$committer = $ident;
	}
}
close(GITVAR);

my $prompting = 0;
if (!defined $from) {
	$from = $author || $committer;
	do {
		$_ = $term->readline("Who should the emails appear to be from? ",
			$from);
	} while (!defined $_);

	$from = $_;
	print "Emails will be sent from: ", $from, "\n";
	$prompting++;
}

if (!@to) {
	do {
		$_ = $term->readline("Who should the emails be sent to? ",
				"");
	} while (!defined $_);
	my $to = $_;
	push @to, split /,/, $to;
	$prompting++;
}

if (!defined $initial_subject && $compose) {
	do {
		$_ = $term->readline("What subject should the emails start with? ",
			$initial_subject);
	} while (!defined $_);
	$initial_subject = $_;
	$prompting++;
}

if (!defined $initial_reply_to && $prompting) {
	do {
		$_= $term->readline("Message-ID to be used as In-Reply-To for the first email? ",
			$initial_reply_to);
	} while (!defined $_);

	$initial_reply_to = $_;
	$initial_reply_to =~ s/(^\s+|\s+$)//g;
}

if (!defined $smtp_server) {
	$smtp_server = "localhost";
}

if ($compose) {
	# Note that this does not need to be secure, but we will make a small
	# effort to have it be unique
	open(C,">",$compose_filename)
		or die "Failed to open for writing $compose_filename: $!";
	print C "From \n";
	printf C "Subject: %s\n\n", $initial_subject;
	printf C <<EOT;
GIT: Please enter your email below.
GIT: Lines beginning in "GIT: " will be removed.
GIT: Consider including an overall diffstat or table of contents
GIT: for the patch you are writing.

EOT
	close(C);

	my $editor = $ENV{EDITOR};
	$editor = 'vi' unless defined $editor;
	system($editor, $compose_filename);

	open(C2,">",$compose_filename . ".final")
		or die "Failed to open $compose_filename.final : " . $!;

	open(C,"<",$compose_filename)
		or die "Failed to open $compose_filename : " . $!;

	while(<C>) {
		next if m/^GIT: /;
		print C2 $_;
	}
	close(C);
	close(C2);

	do {
		$_ = $term->readline("Send this email? (y|n) ");
	} while (!defined $_);

	if (uc substr($_,0,1) ne 'Y') {
		cleanup_compose_files();
		exit(0);
	}

	@files = ($compose_filename . ".final");
}


# Now that all the defaults are set, process the rest of the command line
# arguments and collect up the files that need to be processed.
for my $f (@ARGV) {
	if (-d $f) {
		opendir(DH,$f)
			or die "Failed to opendir $f: $!";

		push @files, grep { -f $_ } map { +$f . "/" . $_ }
				sort readdir(DH);

	} elsif (-f $f) {
		push @files, $f;

	} else {
		print STDERR "Skipping $f - not found.\n";
	}
}

if (@files) {
	print $_,"\n" for @files;
} else {
	print <<EOT;
git-send-email [options] <file | directory> [... file | directory ]
Options:
   --from         Specify the "From:" line of the email to be sent.

   --to           Specify the primary "To:" line of the email.

   --compose      Use \$EDITOR to edit an introductory message for the
                  patch series.

   --subject      Specify the initial "Subject:" line.
                  Only necessary if --compose is also set.  If --compose
		  is not set, this will be prompted for.

   --in-reply-to  Specify the first "In-Reply-To:" header line.
                  Only used if --compose is also set.  If --compose is not
		  set, this will be prompted for.

   --chain-reply-to If set, the replies will all be to the previous
                  email sent, rather than to the first email sent.
                  Defaults to on.

   --smtp-server  If set, specifies the outgoing SMTP server to use.
                  Defaults to localhost.

Error: Please specify a file or a directory on the command line.
EOT
	exit(1);
}

# Variables we set as part of the loop over files
our ($message_id, $cc, %mail, $subject, $reply_to, $message);


# Usually don't need to change anything below here.

# we make a "fake" message id by taking the current number
# of seconds since the beginning of Unix time and tacking on
# a random number to the end, in case we are called quicker than
# 1 second since the last time we were called.

# We'll setup a template for the message id, using the "from" address:
my $message_id_from = Email::Valid->address($from);
my $message_id_template = "<%s-git-send-email-$message_id_from>";

sub make_message_id
{
	my $date = `date "+\%s"`;
	chomp($date);
	my $pseudo_rand = int (rand(4200));
	$message_id = sprintf $message_id_template, "$date$pseudo_rand";
	#print "new message id = $message_id\n"; # Was useful for debugging
}



$cc = "";

sub send_message
{
	my $to = join (", ", unique_email_list(@to));

	%mail = (	To	=>	$to,
			From	=>	$from,
			CC	=>	$cc,
			Subject	=>	$subject,
			Message	=>	$message,
			'Reply-to'	=>	$from,
			'In-Reply-To'	=>	$reply_to,
			'Message-ID'	=>	$message_id,
			'X-Mailer'	=>	"git-send-email",
		);

	$mail{smtp} = $smtp_server;
	$mailcfg{mime} = 0;

	#print Data::Dumper->Dump([\%mail],[qw(*mail)]);

	sendmail(%mail) or die $Mail::Sendmail::error;

	print "OK. Log says:\n", $Mail::Sendmail::log;
	print "\n\n"
}


$reply_to = $initial_reply_to;
make_message_id();
$subject = $initial_subject;

foreach my $t (@files) {
	my $F = $t;
	open(F,"<",$t) or die "can't open file $t";

	@cc = ();
	my $found_mbox = 0;
	my $header_done = 0;
	$message = "";
	while(<F>) {
		if (!$header_done) {
			$found_mbox = 1, next if (/^From /);
			chomp;

			if ($found_mbox) {
				if (/^Subject:\s+(.*)$/) {
					$subject = $1;

				} elsif (/^(Cc|From):\s+(.*)$/) {
					printf("(mbox) Adding cc: %s from line '%s'\n",
						$2, $_);
					push @cc, $2;
				}

			} else {
				# In the traditional
				# "send lots of email" format,
				# line 1 = cc
				# line 2 = subject
				# So let's support that, too.
				if (@cc == 0) {
					printf("(non-mbox) Adding cc: %s from line '%s'\n",
						$_, $_);

					push @cc, $_;

				} elsif (!defined $subject) {
					$subject = $_;
				}
			}

			# A whitespace line will terminate the headers
			if (m/^\s*$/) {
				$header_done = 1;
			}
		} else {
			$message .=  $_;
			if (/^Signed-off-by: (.*)$/i) {
				my $c = $1;
				chomp $c;
				push @cc, $c;
				printf("(sob) Adding cc: %s from line '%s'\n",
					$c, $_);
			}
		}
	}
	close F;

	$cc = join(", ", unique_email_list(@cc));

	send_message();

	# set up for the next message
	if ($chain_reply_to || length($reply_to) == 0) {
		$reply_to = $message_id;
	}
	make_message_id();
}

if ($compose) {
	cleanup_compose_files();
}

sub cleanup_compose_files() {
	unlink($compose_filename, $compose_filename . ".final");

}



sub unique_email_list(@) {
	my %seen;
	my @emails;

	foreach my $entry (@_) {
		my $clean = Email::Valid->address($entry);
		next if $seen{$clean}++;
		push @emails, $entry;
	}
	return @emails;
}
