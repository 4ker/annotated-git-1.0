/*
 * GIT - The information manager from hell
 *
 * Copyright (C) Linus Torvalds, 2005
 */
#include "cache.h"

#ifndef DEFAULT_GIT_TEMPLATE_DIR
#define DEFAULT_GIT_TEMPLATE_DIR "/usr/share/git-core/templates/"
#endif

static void safe_create_dir(const char *dir)
{
	if (mkdir(dir, 0777) < 0) {
		if (errno != EEXIST) {
			perror(dir);
			exit(1);
		}
	}
}

static int copy_file(const char *dst, const char *src, int mode)
{
	int fdi, fdo, status;

	mode = (mode & 0111) ? 0777 : 0666;
	if ((fdi = open(src, O_RDONLY)) < 0)
		return fdi;
	if ((fdo = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode)) < 0) {
		close(fdi);
		return fdo;
	}
	status = copy_fd(fdi, fdo);
	close(fdo);
	return status;
}

static void copy_templates_1(char *path, int baselen,
			     char *template, int template_baselen,
			     DIR *dir)
{
	struct dirent *de;

	/* Note: if ".git/hooks" file exists in the repository being
	 * re-initialized, /etc/core-git/templates/hooks/update would
	 * cause git-init-db to fail here.  I think this is sane but
	 * it means that the set of templates we ship by default, along
	 * with the way the namespace under .git/ is organized, should
	 * be really carefully chosen.
	 */
	safe_create_dir(path);
	while ((de = readdir(dir)) != NULL) {
		struct stat st_git, st_template;
		int namelen;
		int exists = 0;

		if (de->d_name[0] == '.')
			continue;
		namelen = strlen(de->d_name);
		if ((PATH_MAX <= baselen + namelen) ||
		    (PATH_MAX <= template_baselen + namelen))
			die("insanely long template name %s", de->d_name);
		memcpy(path + baselen, de->d_name, namelen+1);
		memcpy(template + template_baselen, de->d_name, namelen+1);
		if (lstat(path, &st_git)) {
			if (errno != ENOENT)
				die("cannot stat %s", path);
		}
		else
			exists = 1;

		if (lstat(template, &st_template))
			die("cannot stat template %s", template);

		if (S_ISDIR(st_template.st_mode)) {
			DIR *subdir = opendir(template);
			int baselen_sub = baselen + namelen;
			int template_baselen_sub = template_baselen + namelen;
			if (!subdir)
				die("cannot opendir %s", template);
			path[baselen_sub++] =
				template[template_baselen_sub++] = '/';
			path[baselen_sub] =
				template[template_baselen_sub] = 0;
			copy_templates_1(path, baselen_sub,
					 template, template_baselen_sub,
					 subdir);
			closedir(subdir);
		}
		else if (exists)
			continue;
		else if (S_ISLNK(st_template.st_mode)) {
			char lnk[256];
			int len;
			len = readlink(template, lnk, sizeof(lnk));
			if (len < 0)
				die("cannot readlink %s", template);
			if (sizeof(lnk) <= len)
				die("insanely long symlink %s", template);
			lnk[len] = 0;
			if (symlink(lnk, path))
				die("cannot symlink %s %s", lnk, path);
		}
		else if (S_ISREG(st_template.st_mode)) {
			if (copy_file(path, template, st_template.st_mode))
				die("cannot copy %s to %s", template, path);
		}
		else
			error("ignoring template %s", template);
	}
}

static void copy_templates(const char *git_dir, int len, char *template_dir)
{
	char path[PATH_MAX];
	char template_path[PATH_MAX];
	int template_len;
	DIR *dir;

	if (!template_dir)
		template_dir = DEFAULT_GIT_TEMPLATE_DIR;
	strcpy(template_path, template_dir);
	template_len = strlen(template_path);
	if (template_path[template_len-1] != '/') {
		template_path[template_len++] = '/';
		template_path[template_len] = 0;
	}
	dir = opendir(template_path);
	if (!dir) {
		fprintf(stderr, "warning: templates not found %s\n",
			template_dir);
		return;
	}

	/* Make sure that template is from the correct vintage */
	strcpy(template_path + template_len, "config");
	repository_format_version = 0;
	git_config_from_file(check_repository_format_version,
			     template_path);
	template_path[template_len] = 0;

	if (repository_format_version &&
	    repository_format_version != GIT_REPO_VERSION) {
		fprintf(stderr, "warning: not copying templates of "
			"a wrong format version %d from '%s'\n",
			repository_format_version,
			template_dir);
		closedir(dir);
		return;
	}

	memcpy(path, git_dir, len);
	path[len] = 0;
	copy_templates_1(path, len,
			 template_path, template_len,
			 dir);
	closedir(dir);
}

static void create_default_files(const char *git_dir, char *template_path)
{
	unsigned len = strlen(git_dir);
	static char path[PATH_MAX];
	unsigned char sha1[20];
	struct stat st1;
	char repo_version_string[10];

	if (len > sizeof(path)-50)
		die("insane git directory %s", git_dir);
	memcpy(path, git_dir, len);

	if (len && path[len-1] != '/')
		path[len++] = '/';

	/*
	 * Create .git/refs/{heads,tags}
	 */
	strcpy(path + len, "refs");
	safe_create_dir(path);
	strcpy(path + len, "refs/heads");
	safe_create_dir(path);
	strcpy(path + len, "refs/tags");
	safe_create_dir(path);

	/* First copy the templates -- we might have the default
	 * config file there, in which case we would want to read
	 * from it after installing.
	 */
	path[len] = 0;
	copy_templates(path, len, template_path);

	git_config(git_default_config);

	/*
	 * Create the default symlink from ".git/HEAD" to the "master"
	 * branch, if it does not exist yet.
	 */
	strcpy(path + len, "HEAD");
	if (read_ref(path, sha1) < 0) {
		if (create_symref(path, "refs/heads/master") < 0)
			exit(1);
	}

	/* This forces creation of new config file */
	sprintf(repo_version_string, "%d", GIT_REPO_VERSION);
	git_config_set("core.repositoryformatversion", repo_version_string);

	path[len] = 0;
	strcpy(path + len, "config");

	/* Check filemode trustability */
	if (!lstat(path, &st1)) {
		struct stat st2;
		int filemode = (!chmod(path, st1.st_mode ^ S_IXUSR) &&
				!lstat(path, &st2) &&
				st1.st_mode != st2.st_mode);
		git_config_set("core.filemode",
			       filemode ? "true" : "false");
	}
}

static const char init_db_usage[] =
"git-init-db [--template=<template-directory>]";

/*
 * If you want to, you can share the DB area with any number of branches.
 * That has advantages: you can save space by sharing all the SHA1 objects.
 * On the other hand, it might just make lookup slower and messier. You
 * be the judge.  The default case is to have one DB per managed directory.
 */
int main(int argc, char **argv)
{
	const char *git_dir;
	const char *sha1_dir;
	char *path, *template_dir = NULL;
	int len, i;

	for (i = 1; i < argc; i++, argv++) {
		char *arg = argv[1];
		if (!strncmp(arg, "--template=", 11))
			template_dir = arg+11;
		else
			die(init_db_usage);
	}

	/*
	 * Set up the default .git directory contents
	 */
	git_dir = getenv(GIT_DIR_ENVIRONMENT);
	if (!git_dir) {
		git_dir = DEFAULT_GIT_DIR_ENVIRONMENT;
		fprintf(stderr, "defaulting to local storage area\n");
	}
	safe_create_dir(git_dir);

	/* Check to see if the repository version is right.
	 * Note that a newly created repository does not have
	 * config file, so this will not fail.  What we are catching
	 * is an attempt to reinitialize new repository with an old tool.
	 */
	check_repository_format();

	create_default_files(git_dir, template_dir);

	/*
	 * And set up the object store.
	 */
	sha1_dir = get_object_directory();
	len = strlen(sha1_dir);
	path = xmalloc(len + 40);
	memcpy(path, sha1_dir, len);

	safe_create_dir(sha1_dir);
	strcpy(path+len, "/pack");
	safe_create_dir(path);
	strcpy(path+len, "/info");
	safe_create_dir(path);
	return 0;
}
