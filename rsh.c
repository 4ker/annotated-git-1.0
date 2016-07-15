#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "rsh.h"
#include "quote.h"
#include "cache.h"

#define COMMAND_SIZE 4096

/*
 * Append a string to a string buffer, with or without shell quoting.
 * Return true if the buffer overflowed.
 */
static int add_to_string(char **ptrp, int *sizep, const char *str, int quote)
{
	char *p = *ptrp;
	int size = *sizep;
	int oc;
	int err = 0;

	if ( quote ) {
		oc = sq_quote_buf(p, size, str);
	} else {
		oc = strlen(str);
		memcpy(p, str, (oc >= size) ? size-1 : oc);
	}

	if ( oc >= size ) {
		err = 1;
		oc = size-1;
	}

	*ptrp  += oc;
	**ptrp  = '\0';
	*sizep -= oc;
	return err;
}

int setup_connection(int *fd_in, int *fd_out, const char *remote_prog, 
		     char *url, int rmt_argc, char **rmt_argv)
{
	char *host;
	char *path;
	int sv[2];
	char command[COMMAND_SIZE];
	char *posn;
	int sizen;
	int of;
	int i;

	if (!strcmp(url, "-")) {
		*fd_in = 0;
		*fd_out = 1;
		return 0;
	}

	host = strstr(url, "//");
	if (host) {
		host += 2;
		path = strchr(host, '/');
	} else {
		host = url;
		path = strchr(host, ':');
		if (path)
			*(path++) = '\0';
	}
	if (!path) {
		return error("Bad URL: %s", url);
	}
	/* $GIT_RSH <host> "env GIT_DIR=<path> <remote_prog> <args...>" */
	sizen = COMMAND_SIZE;
	posn = command;
	of = 0;
	of |= add_to_string(&posn, &sizen, "env ", 0);
	of |= add_to_string(&posn, &sizen, GIT_DIR_ENVIRONMENT "=", 0);
	of |= add_to_string(&posn, &sizen, path, 1);
	of |= add_to_string(&posn, &sizen, " ", 0);
	of |= add_to_string(&posn, &sizen, remote_prog, 1);

	for ( i = 0 ; i < rmt_argc ; i++ ) {
		of |= add_to_string(&posn, &sizen, " ", 0);
		of |= add_to_string(&posn, &sizen, rmt_argv[i], 1);
	}

	of |= add_to_string(&posn, &sizen, " -", 0);

	if ( of )
		return error("Command line too long");

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return error("Couldn't create socket");

	if (!fork()) {
		const char *ssh, *ssh_basename;
		ssh = getenv("GIT_SSH");
		if (!ssh) ssh = "ssh";
		ssh_basename = strrchr(ssh, '/');
		if (!ssh_basename)
			ssh_basename = ssh;
		else
			ssh_basename++;
		close(sv[1]);
		dup2(sv[0], 0);
		dup2(sv[0], 1);
		execlp(ssh, ssh_basename, host, command, NULL);
	}
	close(sv[0]);
	*fd_in = sv[1];
	*fd_out = sv[1];
	return 0;
}
