/*
 * Totally braindamaged mbox splitter program.
 *
 * It just splits a mbox into a list of files: "0001" "0002" ..
 * so you can process them further from there.
 */
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "cache.h"

static const char git_mailsplit_usage[] =
"git-mailsplit [-d<prec>] [-f<n>] [-b] -o<directory> <mbox>...";

static int is_from_line(const char *line, int len)
{
	const char *colon;

	if (len < 20 || memcmp("From ", line, 5))
		return 0;

	colon = line + len - 2;
	line += 5;
	for (;;) {
		if (colon < line)
			return 0;
		if (*--colon == ':')
			break;
	}

	if (!isdigit(colon[-4]) ||
	    !isdigit(colon[-2]) ||
	    !isdigit(colon[-1]) ||
	    !isdigit(colon[ 1]) ||
	    !isdigit(colon[ 2]))
		return 0;

	/* year */
	if (strtol(colon+3, NULL, 10) <= 90)
		return 0;

	/* Ok, close enough */
	return 1;
}

/* Could be as small as 64, enough to hold a Unix "From " line. */
static char buf[4096];

/* Called with the first line (potentially partial)
 * already in buf[] -- normally that should begin with
 * the Unix "From " line.  Write it into the specified
 * file.
 */
static int split_one(FILE *mbox, const char *name, int allow_bare)
{
	FILE *output = NULL;
	int len = strlen(buf);
	int fd;
	int status = 0;
	int is_bare = !is_from_line(buf, len);

	if (is_bare && !allow_bare)
		goto corrupt;

	fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (fd < 0)
		die("cannot open output file %s", name);
	output = fdopen(fd, "w");

	/* Copy it out, while searching for a line that begins with
	 * "From " and having something that looks like a date format.
	 */
	for (;;) {
		int is_partial = (buf[len-1] != '\n');

		if (fputs(buf, output) == EOF)
			die("cannot write output");

		if (fgets(buf, sizeof(buf), mbox) == NULL) {
			if (feof(mbox)) {
				status = 1;
				break;
			}
			die("cannot read mbox");
		}
		len = strlen(buf);
		if (!is_partial && !is_bare && is_from_line(buf, len))
			break; /* done with one message */
	}
	fclose(output);
	return status;

 corrupt:
	if (output)
		fclose(output);
	unlink(name);
	fprintf(stderr, "corrupt mailbox\n");
	exit(1);
}

int main(int argc, const char **argv)
{
	int nr = 0, nr_prec = 4;
	int allow_bare = 0;
	const char *dir = NULL;
	const char **argp;
	static const char *stdin_only[] = { "-", NULL };
	char *name;

	for (argp = argv+1; *argp; argp++) {
		const char *arg = *argp;

		if (arg[0] != '-')
			break;
		/* do flags here */
		if ( arg[1] == 'd' ) {
			nr_prec = strtol(arg+2, NULL, 10);
			if (nr_prec < 3 || 10 <= nr_prec)
				usage(git_mailsplit_usage);
			continue;
		} else if ( arg[1] == 'f' ) {
			nr = strtol(arg+2, NULL, 10);
		} else if ( arg[1] == 'b' && !arg[2] ) {
			allow_bare = 1;
		} else if ( arg[1] == 'o' && arg[2] ) {
			dir = arg+2;
		} else if ( arg[1] == '-' && !arg[2] ) {
			argp++;	/* -- marks end of options */
			break;
		} else {
			die("unknown option: %s", arg);
		}
	}

	if ( !dir ) {
		/* Backwards compatibility: if no -o specified, accept
		   <mbox> <dir> or just <dir> */
		switch (argc - (argp-argv)) {
		case 1:
			dir = argp[0];
			argp = stdin_only;
			break;
		case 2:
			stdin_only[0] = argp[0];
			dir = argp[1];
			argp = stdin_only;
			break;
		default:
			usage(git_mailsplit_usage);
		}
	} else {
		/* New usage: if no more argument, parse stdin */
		if ( !*argp )
			argp = stdin_only;
	}

	name = xmalloc(strlen(dir) + 2 + 3 * sizeof(nr));

	while (*argp) {
		const char *file = *argp++;
		FILE *f = !strcmp(file, "-") ? stdin : fopen(file, "rt");
		int file_done = 0;

		if ( !f )
			die ("cannot open mbox %s", file);

		if (fgets(buf, sizeof(buf), f) == NULL)
			die("cannot read mbox %s", file);

		while (!file_done) {
			sprintf(name, "%s/%0*d", dir, nr_prec, ++nr);
			file_done = split_one(f, name, allow_bare);
		}

		if (f != stdin)
			fclose(f);
	}

	printf("%d\n", nr);
	return 0;
}
