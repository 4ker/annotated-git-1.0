/*
 * Helper functions for tree diff generation
 */
#include "cache.h"
#include "diff.h"

// What paths are we interested in?
static int nr_paths = 0;
static const char **paths = NULL;
static int *pathlens = NULL;

static void update_tree_entry(struct tree_desc *desc)
{
	void *buf = desc->buf;
	unsigned long size = desc->size;
	int len = strlen(buf) + 1 + 20;

	if (size < len)
		die("corrupt tree file");
	desc->buf = buf + len;
	desc->size = size - len;
}

static const unsigned char *extract(struct tree_desc *desc, const char **pathp, unsigned int *modep)
{
	void *tree = desc->buf;
	unsigned long size = desc->size;
	int len = strlen(tree)+1;
	const unsigned char *sha1 = tree + len;
	const char *path = strchr(tree, ' ');
	unsigned int mode;

	if (!path || size < len + 20 || sscanf(tree, "%o", &mode) != 1)
		die("corrupt tree file");
	*pathp = path+1;
	*modep = DIFF_FILE_CANON_MODE(mode);
	return sha1;
}

static char *malloc_base(const char *base, const char *path, int pathlen)
{
	int baselen = strlen(base);
	char *newbase = xmalloc(baselen + pathlen + 2);
	memcpy(newbase, base, baselen);
	memcpy(newbase + baselen, path, pathlen);
	memcpy(newbase + baselen + pathlen, "/", 2);
	return newbase;
}

static int show_entry(struct diff_options *opt, const char *prefix, struct tree_desc *desc, const char *base);

static int compare_tree_entry(struct tree_desc *t1, struct tree_desc *t2, const char *base, struct diff_options *opt)
{
	unsigned mode1, mode2;
	const char *path1, *path2;
	const unsigned char *sha1, *sha2;
	int cmp, pathlen1, pathlen2;

	sha1 = extract(t1, &path1, &mode1);
	sha2 = extract(t2, &path2, &mode2);

	pathlen1 = strlen(path1);
	pathlen2 = strlen(path2);
	cmp = base_name_compare(path1, pathlen1, mode1, path2, pathlen2, mode2);
	if (cmp < 0) {
		show_entry(opt, "-", t1, base);
		return -1;
	}
	if (cmp > 0) {
		show_entry(opt, "+", t2, base);
		return 1;
	}
	if (!opt->find_copies_harder &&
	    !memcmp(sha1, sha2, 20) && mode1 == mode2)
		return 0;

	/*
	 * If the filemode has changed to/from a directory from/to a regular
	 * file, we need to consider it a remove and an add.
	 */
	if (S_ISDIR(mode1) != S_ISDIR(mode2)) {
		show_entry(opt, "-", t1, base);
		show_entry(opt, "+", t2, base);
		return 0;
	}

	if (opt->recursive && S_ISDIR(mode1)) {
		int retval;
		char *newbase = malloc_base(base, path1, pathlen1);
		if (opt->tree_in_recursive)
			opt->change(opt, mode1, mode2,
				    sha1, sha2, base, path1);
		retval = diff_tree_sha1(sha1, sha2, newbase, opt);
		free(newbase);
		return retval;
	}

	opt->change(opt, mode1, mode2, sha1, sha2, base, path1);
	return 0;
}

static int interesting(struct tree_desc *desc, const char *base)
{
	const char *path;
	unsigned mode;
	int i;
	int baselen, pathlen;

	if (!nr_paths)
		return 1;

	(void)extract(desc, &path, &mode);

	pathlen = strlen(path);
	baselen = strlen(base);

	for (i=0; i < nr_paths; i++) {
		const char *match = paths[i];
		int matchlen = pathlens[i];

		if (baselen >= matchlen) {
			/* If it doesn't match, move along... */
			if (strncmp(base, match, matchlen))
				continue;

			/* The base is a subdirectory of a path which was specified. */
			return 1;
		}

		/* Does the base match? */
		if (strncmp(base, match, baselen))
			continue;

		match += baselen;
		matchlen -= baselen;

		if (pathlen > matchlen)
			continue;

		if (matchlen > pathlen) {
			if (match[pathlen] != '/')
				continue;
			if (!S_ISDIR(mode))
				continue;
		}

		if (strncmp(path, match, pathlen))
			continue;

		return 1;
	}
	return 0; /* No matches */
}

/* A whole sub-tree went away or appeared */
static void show_tree(struct diff_options *opt, const char *prefix, struct tree_desc *desc, const char *base)
{
	while (desc->size) {
		if (interesting(desc, base))
			show_entry(opt, prefix, desc, base);
		update_tree_entry(desc);
	}
}

/* A file entry went away or appeared */
static int show_entry(struct diff_options *opt, const char *prefix, struct tree_desc *desc, const char *base)
{
	unsigned mode;
	const char *path;
	const unsigned char *sha1 = extract(desc, &path, &mode);

	if (opt->recursive && S_ISDIR(mode)) {
		char type[20];
		char *newbase = malloc_base(base, path, strlen(path));
		struct tree_desc inner;
		void *tree;

		tree = read_sha1_file(sha1, type, &inner.size);
		if (!tree || strcmp(type, "tree"))
			die("corrupt tree sha %s", sha1_to_hex(sha1));

		inner.buf = tree;
		show_tree(opt, prefix, &inner, newbase);

		free(tree);
		free(newbase);
		return 0;
	}

	opt->add_remove(opt, prefix[0], mode, sha1, base, path);
	return 0;
}

int diff_tree(struct tree_desc *t1, struct tree_desc *t2, const char *base, struct diff_options *opt)
{
	while (t1->size | t2->size) {
		if (nr_paths && t1->size && !interesting(t1, base)) {
			update_tree_entry(t1);
			continue;
		}
		if (nr_paths && t2->size && !interesting(t2, base)) {
			update_tree_entry(t2);
			continue;
		}
		if (!t1->size) {
			show_entry(opt, "+", t2, base);
			update_tree_entry(t2);
			continue;
		}
		if (!t2->size) {
			show_entry(opt, "-", t1, base);
			update_tree_entry(t1);
			continue;
		}
		switch (compare_tree_entry(t1, t2, base, opt)) {
		case -1:
			update_tree_entry(t1);
			continue;
		case 0:
			update_tree_entry(t1);
			/* Fallthrough */
		case 1:
			update_tree_entry(t2);
			continue;
		}
		die("git-diff-tree: internal error");
	}
	return 0;
}

int diff_tree_sha1(const unsigned char *old, const unsigned char *new, const char *base, struct diff_options *opt)
{
	void *tree1, *tree2;
	struct tree_desc t1, t2;
	int retval;

	tree1 = read_object_with_reference(old, "tree", &t1.size, NULL);
	if (!tree1)
		die("unable to read source tree (%s)", sha1_to_hex(old));
	tree2 = read_object_with_reference(new, "tree", &t2.size, NULL);
	if (!tree2)
		die("unable to read destination tree (%s)", sha1_to_hex(new));
	t1.buf = tree1;
	t2.buf = tree2;
	retval = diff_tree(&t1, &t2, base, opt);
	free(tree1);
	free(tree2);
	return retval;
}

static int count_paths(const char **paths)
{
	int i = 0;
	while (*paths++)
		i++;
	return i;
}

void diff_tree_setup_paths(const char **p)
{
	if (p) {
		int i;

		paths = p;
		nr_paths = count_paths(paths);
		pathlens = xmalloc(nr_paths * sizeof(int));
		for (i=0; i<nr_paths; i++)
			pathlens[i] = strlen(paths[i]);
	}
}
