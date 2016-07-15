/*
 * GIT - The information manager from hell
 *
 * Copyright (C) Linus Torvalds, 2005
 * Copyright (C) Johannes Schindelin, 2005
 *
 */
#include "cache.h"
#include <regex.h>

#define MAXNAME (256)

static FILE *config_file;
static const char *config_file_name;
static int config_linenr;
static int get_next_char(void)
{
	int c;
	FILE *f;

	c = '\n';
	if ((f = config_file) != NULL) {
		c = fgetc(f);
		if (c == '\r') {
			/* DOS like systems */
			c = fgetc(f);
			if (c != '\n') {
				ungetc(c, f);
				c = '\r';
			}
		}
		if (c == '\n')
			config_linenr++;
		if (c == EOF) {
			config_file = NULL;
			c = '\n';
		}
	}
	return c;
}

static char *parse_value(void)
{
	static char value[1024];
	int quote = 0, comment = 0, len = 0, space = 0;

	for (;;) {
		int c = get_next_char();
		if (len >= sizeof(value))
			return NULL;
		if (c == '\n') {
			if (quote)
				return NULL;
			value[len] = 0;
			return value;
		}
		if (comment)
			continue;
		if (isspace(c) && !quote) {
			space = 1;
			continue;
		}
		if (space) {
			if (len)
				value[len++] = ' ';
			space = 0;
		}
		if (c == '\\') {
			c = get_next_char();
			switch (c) {
			case '\n':
				continue;
			case 't':
				c = '\t';
				break;
			case 'b':
				c = '\b';
				break;
			case 'n':
				c = '\n';
				break;
			/* Some characters escape as themselves */
			case '\\': case '"':
				break;
			/* Reject unknown escape sequences */
			default:
				return NULL;
			}
			value[len++] = c;
			continue;
		}
		if (c == '"') {
			quote = 1-quote;
			continue;
		}
		if (!quote) {
			if (c == ';' || c == '#') {
				comment = 1;
				continue;
			}
		}
		value[len++] = c;
	}
}

static int get_value(config_fn_t fn, char *name, unsigned int len)
{
	int c;
	char *value;

	/* Get the full name */
	for (;;) {
		c = get_next_char();
		if (c == EOF)
			break;
		if (!isalnum(c))
			break;
		name[len++] = tolower(c);
		if (len >= MAXNAME)
			return -1;
	}
	name[len] = 0;
	while (c == ' ' || c == '\t')
		c = get_next_char();

	value = NULL;
	if (c != '\n') {
		if (c != '=')
			return -1;
		value = parse_value();
		if (!value)
			return -1;
	}
	return fn(name, value);
}

static int get_base_var(char *name)
{
	int baselen = 0;

	for (;;) {
		int c = get_next_char();
		if (c == EOF)
			return -1;
		if (c == ']')
			return baselen;
		if (!isalnum(c) && c != '.')
			return -1;
		if (baselen > MAXNAME / 2)
			return -1;
		name[baselen++] = tolower(c);
	}
}

static int git_parse_file(config_fn_t fn)
{
	int comment = 0;
	int baselen = 0;
	static char var[MAXNAME];

	for (;;) {
		int c = get_next_char();
		if (c == '\n') {
			/* EOF? */
			if (!config_file)
				return 0;
			comment = 0;
			continue;
		}
		if (comment || isspace(c))
			continue;
		if (c == '#' || c == ';') {
			comment = 1;
			continue;
		}
		if (c == '[') {
			baselen = get_base_var(var);
			if (baselen <= 0)
				break;
			var[baselen++] = '.';
			var[baselen] = 0;
			continue;
		}
		if (!isalpha(c))
			break;
		var[baselen] = tolower(c);
		if (get_value(fn, var, baselen+1) < 0)
			break;
	}
	die("bad config file line %d in %s", config_linenr, config_file_name);
}

int git_config_int(const char *name, const char *value)
{
	if (value && *value) {
		char *end;
		int val = strtol(value, &end, 0);
		if (!*end)
			return val;
	}
	die("bad config value for '%s' in %s", name, config_file_name);
}

int git_config_bool(const char *name, const char *value)
{
	if (!value)
		return 1;
	if (!*value)
		return 0;
	if (!strcasecmp(value, "true"))
		return 1;
	if (!strcasecmp(value, "false"))
		return 0;
	return git_config_int(name, value) != 0;
}

int git_default_config(const char *var, const char *value)
{
	/* This needs a better name */
	if (!strcmp(var, "core.filemode")) {
		trust_executable_bit = git_config_bool(var, value);
		return 0;
	}

	if (!strcmp(var, "core.symrefsonly")) {
		only_use_symrefs = git_config_bool(var, value);
		return 0;
	}

	if (!strcmp(var, "user.name")) {
		strncpy(git_default_name, value, sizeof(git_default_name));
		return 0;
	}

	if (!strcmp(var, "user.email")) {
		strncpy(git_default_email, value, sizeof(git_default_email));
		return 0;
	}

	if (!strcmp(var, "i18n.commitencoding")) {
		strncpy(git_commit_encoding, value, sizeof(git_commit_encoding));
		return 0;
	}

	/* Add other config variables here.. */
	return 0;
}

int git_config_from_file(config_fn_t fn, const char *filename)
{
	int ret;
	FILE *f = fopen(filename, "r");

	ret = -1;
	if (f) {
		config_file = f;
		config_file_name = filename;
		config_linenr = 1;
		ret = git_parse_file(fn);
		fclose(f);
		config_file_name = NULL;
	}
	return ret;
}

int git_config(config_fn_t fn)
{
	return git_config_from_file(fn, git_path("config"));
}

/*
 * Find all the stuff for git_config_set() below.
 */

#define MAX_MATCHES 512

static struct {
	int baselen;
	char* key;
	int do_not_match;
	regex_t* value_regex;
	int multi_replace;
	off_t offset[MAX_MATCHES];
	enum { START, SECTION_SEEN, SECTION_END_SEEN, KEY_SEEN } state;
	int seen;
} store;

static int matches(const char* key, const char* value)
{
	return !strcmp(key, store.key) &&
		(store.value_regex == NULL ||
		 (store.do_not_match ^
		  !regexec(store.value_regex, value, 0, NULL, 0)));
}

static int store_aux(const char* key, const char* value)
{
	switch (store.state) {
	case KEY_SEEN:
		if (matches(key, value)) {
			if (store.seen == 1 && store.multi_replace == 0) {
				fprintf(stderr,
					"Warning: %s has multiple values\n",
					key);
			} else if (store.seen >= MAX_MATCHES) {
				fprintf(stderr, "Too many matches\n");
				return 1;
			}

			store.offset[store.seen] = ftell(config_file);
			store.seen++;
		}
		break;
	case SECTION_SEEN:
		if (strncmp(key, store.key, store.baselen+1)) {
			store.state = SECTION_END_SEEN;
			break;
		} else
			/* do not increment matches: this is no match */
			store.offset[store.seen] = ftell(config_file);
		/* fallthru */
	case SECTION_END_SEEN:
	case START:
		if (matches(key, value)) {
			store.offset[store.seen] = ftell(config_file);
			store.state = KEY_SEEN;
			store.seen++;
		} else if(!strncmp(key, store.key, store.baselen))
			store.state = SECTION_SEEN;
	}
	return 0;
}

static void store_write_section(int fd, const char* key)
{
	write(fd, "[", 1);
	write(fd, key, store.baselen);
	write(fd, "]\n", 2);
}

static void store_write_pair(int fd, const char* key, const char* value)
{
	int i;

	write(fd, "\t", 1);
	write(fd, key+store.baselen+1,
		strlen(key+store.baselen+1));
	write(fd, " = ", 3);
	for (i = 0; value[i]; i++)
		switch (value[i]) {
		case '\n': write(fd, "\\n", 2); break;
		case '\t': write(fd, "\\t", 2); break;
		case '"': case '\\': write(fd, "\\", 1);
		default: write(fd, value+i, 1);
	}
	write(fd, "\n", 1);
}

static int find_beginning_of_line(const char* contents, int size,
	int offset_, int* found_bracket)
{
	int equal_offset = size, bracket_offset = size;
	int offset;

	for (offset = offset_-2; offset > 0 
			&& contents[offset] != '\n'; offset--)
		switch (contents[offset]) {
			case '=': equal_offset = offset; break;
			case ']': bracket_offset = offset; break;
		}
	if (bracket_offset < equal_offset) {
		*found_bracket = 1;
		offset = bracket_offset+1;
	} else
		offset++;

	return offset;
}

int git_config_set(const char* key, const char* value)
{
	return git_config_set_multivar(key, value, NULL, 0);
}

/*
 * If value==NULL, unset in (remove from) config,
 * if value_regex!=NULL, disregard key/value pairs where value does not match.
 * if multi_replace==0, nothing, or only one matching key/value is replaced,
 *     else all matching key/values (regardless how many) are removed,
 *     before the new pair is written.
 *
 * Returns 0 on success.
 *
 * This function does this:
 *
 * - it locks the config file by creating ".git/config.lock"
 *
 * - it then parses the config using store_aux() as validator to find
 *   the position on the key/value pair to replace. If it is to be unset,
 *   it must be found exactly once.
 *
 * - the config file is mmap()ed and the part before the match (if any) is
 *   written to the lock file, then the changed part and the rest.
 *
 * - the config file is removed and the lock file rename()d to it.
 *
 */
int git_config_set_multivar(const char* key, const char* value,
	const char* value_regex, int multi_replace)
{
	int i;
	struct stat st;
	int fd;
	char* config_filename = strdup(git_path("config"));
	char* lock_file = strdup(git_path("config.lock"));
	const char* last_dot = strrchr(key, '.');

	/*
	 * Since "key" actually contains the section name and the real
	 * key name separated by a dot, we have to know where the dot is.
	 */

	if (last_dot == NULL) {	
		fprintf(stderr, "key does not contain a section: %s\n", key);
		return 2;
	}
	store.baselen = last_dot - key;

	store.multi_replace = multi_replace;

	/*
	 * Validate the key and while at it, lower case it for matching.
	 */
	store.key = (char*)malloc(strlen(key)+1);
	for (i = 0; key[i]; i++)
		if (i != store.baselen &&
				((!isalnum(key[i]) && key[i] != '.') ||
				 (i == store.baselen+1 && !isalpha(key[i])))) {
			fprintf(stderr, "invalid key: %s\n", key);
			free(store.key);
			return 1;
		} else
			store.key[i] = tolower(key[i]);
	store.key[i] = 0;

	/*
	 * The lock_file serves a purpose in addition to locking: the new
	 * contents of .git/config will be written into it.
	 */
	fd = open(lock_file, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (fd < 0) {
		fprintf(stderr, "could not lock config file\n");
		free(store.key);
		return -1;
	}

	/*
	 * If .git/config does not exist yet, write a minimal version.
	 */
	if (stat(config_filename, &st)) {
		free(store.key);

		/* if nothing to unset, error out */
		if (value == NULL) {
			close(fd);
			unlink(lock_file);
			return 5;
		}

		store.key = (char*)key;
		store_write_section(fd, key);
		store_write_pair(fd, key, value);
	} else{
		int in_fd;
		char* contents;
		int i, copy_begin, copy_end, new_line = 0;

		if (value_regex == NULL)
			store.value_regex = NULL;
		else {
			if (value_regex[0] == '!') {
				store.do_not_match = 1;
				value_regex++;
			} else
				store.do_not_match = 0;

			store.value_regex = (regex_t*)malloc(sizeof(regex_t));
			if (regcomp(store.value_regex, value_regex,
					REG_EXTENDED)) {
				fprintf(stderr, "Invalid pattern: %s",
					value_regex);
				free(store.value_regex);
				return 6;
			}
		}

		store.offset[0] = 0;
		store.state = START;
		store.seen = 0;

		/*
		 * After this, store.offset will contain the *end* offset
		 * of the last match, or remain at 0 if no match was found.
		 * As a side effect, we make sure to transform only a valid
		 * existing config file.
		 */
		if (git_config(store_aux)) {
			fprintf(stderr, "invalid config file\n");
			free(store.key);
			if (store.value_regex != NULL) {
				regfree(store.value_regex);
				free(store.value_regex);
			}
			return 3;
		}

		free(store.key);
		if (store.value_regex != NULL) {
			regfree(store.value_regex);
			free(store.value_regex);
		}

		/* if nothing to unset, or too many matches, error out */
		if ((store.seen == 0 && value == NULL) ||
				(store.seen > 1 && multi_replace == 0)) {
			close(fd);
			unlink(lock_file);
			return 5;
		}

		in_fd = open(config_filename, O_RDONLY, 0666);
		contents = mmap(NULL, st.st_size, PROT_READ,
			MAP_PRIVATE, in_fd, 0);
		close(in_fd);

		if (store.seen == 0)
			store.seen = 1;

		for (i = 0, copy_begin = 0; i < store.seen; i++) {
			if (store.offset[i] == 0) {
				store.offset[i] = copy_end = st.st_size;
			} else if (store.state != KEY_SEEN) {
				copy_end = store.offset[i];
			} else
				copy_end = find_beginning_of_line(
					contents, st.st_size,
					store.offset[i]-2, &new_line);

			/* write the first part of the config */
			if (copy_end > copy_begin) {
				write(fd, contents + copy_begin,
				copy_end - copy_begin);
				if (new_line)
					write(fd, "\n", 1);
			}
			copy_begin = store.offset[i];
		}

		/* write the pair (value == NULL means unset) */
		if (value != NULL) {
			if (store.state == START)
				store_write_section(fd, key);
			store_write_pair(fd, key, value);
		}

		/* write the rest of the config */
		if (copy_begin < st.st_size)
			write(fd, contents + copy_begin,
				st.st_size - copy_begin);

		munmap(contents, st.st_size);
		unlink(config_filename);
	}

	close(fd);

	if (rename(lock_file, config_filename) < 0) {
		fprintf(stderr, "Could not rename the lock file?\n");
		return 4;
	}

	return 0;
}


