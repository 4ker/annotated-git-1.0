#include "cache.h"
#include "run-command.h"
#include <sys/wait.h>

int run_command_v_opt(int argc, char **argv, int flags)
{
	pid_t pid = fork();

	if (pid < 0)
		return -ERR_RUN_COMMAND_FORK;
	if (!pid) {
		if (flags & RUN_COMMAND_NO_STDIO) {
			int fd = open("/dev/null", O_RDWR);
			dup2(fd, 0);
			dup2(fd, 1);
			close(fd);			
		}
		execvp(argv[0], (char *const*) argv);
		die("exec %s failed.", argv[0]);
	}
	for (;;) {
		int status, code;
		int retval = waitpid(pid, &status, 0);

		if (retval < 0) {
			if (errno == EINTR)
				continue;
			error("waitpid failed (%s)", strerror(retval));
			return -ERR_RUN_COMMAND_WAITPID;
		}
		if (retval != pid)
			return -ERR_RUN_COMMAND_WAITPID_WRONG_PID;
		if (WIFSIGNALED(status))
			return -ERR_RUN_COMMAND_WAITPID_SIGNAL;

		if (!WIFEXITED(status))
			return -ERR_RUN_COMMAND_WAITPID_NOEXIT;
		code = WEXITSTATUS(status);
		if (code)
			return -code;
		return 0;
	}
}

int run_command_v(int argc, char **argv)
{
	return run_command_v_opt(argc, argv, 0);
}

int run_command(const char *cmd, ...)
{
	int argc;
	char *argv[MAX_RUN_COMMAND_ARGS];
	const char *arg;
	va_list param;

	va_start(param, cmd);
	argv[0] = (char*) cmd;
	argc = 1;
	while (argc < MAX_RUN_COMMAND_ARGS) {
		arg = argv[argc++] = va_arg(param, char *);
		if (!arg)
			break;
	}
	va_end(param);
	if (MAX_RUN_COMMAND_ARGS <= argc)
		return error("too many args to run %s", cmd);
	return run_command_v_opt(argc, argv, 0);
}
