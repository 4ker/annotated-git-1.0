#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "../git-compat-util.h"

void *gitfakemmap(void *start, size_t length, int prot , int flags, int fd, off_t offset)
{
	int n = 0;

	if (start != NULL || !(flags & MAP_PRIVATE))
		die("Invalid usage of gitfakemmap.");

	if (lseek(fd, offset, SEEK_SET) < 0) {
		errno = EINVAL;
		return MAP_FAILED;
	}

	start = xmalloc(length);
	if (start == NULL) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

	while (n < length) {
		int count = read(fd, start+n, length-n);

		if (count == 0) {
			memset(start+n, 0, length-n);
			break;
		}

		if (count < 0) {
			free(start);
			errno = EACCES;
			return MAP_FAILED;
		}

		n += count;
	}

	return start;
}

int gitfakemunmap(void *start, size_t length)
{
	free(start);
	return 0;
}

