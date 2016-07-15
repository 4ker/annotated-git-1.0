#include "cache.h"
#include "tag.h"
#include "commit.h"
#include "tree.h"
#include "blob.h"

static int find_short_object_filename(int len, const char *name, unsigned char *sha1)
{
	struct alternate_object_database *alt;
	char hex[40];
	int found = 0;
	static struct alternate_object_database *fakeent;

	if (!fakeent) {
		const char *objdir = get_object_directory();
		int objdir_len = strlen(objdir);
		int entlen = objdir_len + 43;
		fakeent = xmalloc(sizeof(*fakeent) + entlen);
		memcpy(fakeent->base, objdir, objdir_len);
		fakeent->name = fakeent->base + objdir_len + 1;
		fakeent->name[-1] = '/';
	}
	fakeent->next = alt_odb_list;

	sprintf(hex, "%.2s", name);
	for (alt = fakeent; alt && found < 2; alt = alt->next) {
		struct dirent *de;
		DIR *dir;
		sprintf(alt->name, "%.2s/", name);
		dir = opendir(alt->base);
		if (!dir)
			continue;
		while ((de = readdir(dir)) != NULL) {
			if (strlen(de->d_name) != 38)
				continue;
			if (memcmp(de->d_name, name + 2, len - 2))
				continue;
			if (!found) {
				memcpy(hex + 2, de->d_name, 38);
				found++;
			}
			else if (memcmp(hex + 2, de->d_name, 38)) {
				found = 2;
				break;
			}
		}
		closedir(dir);
	}
	if (found == 1)
		return get_sha1_hex(hex, sha1) == 0;
	return found;
}

static int match_sha(unsigned len, const unsigned char *a, const unsigned char *b)
{
	do {
		if (*a != *b)
			return 0;
		a++;
		b++;
		len -= 2;
	} while (len > 1);
	if (len)
		if ((*a ^ *b) & 0xf0)
			return 0;
	return 1;
}

static int find_short_packed_object(int len, const unsigned char *match, unsigned char *sha1)
{
	struct packed_git *p;
	unsigned char found_sha1[20];
	int found = 0;

	prepare_packed_git();
	for (p = packed_git; p && found < 2; p = p->next) {
		unsigned num = num_packed_objects(p);
		unsigned first = 0, last = num;
		while (first < last) {
			unsigned mid = (first + last) / 2;
			unsigned char now[20];
			int cmp;

			nth_packed_object_sha1(p, mid, now);
			cmp = memcmp(match, now, 20);
			if (!cmp) {
				first = mid;
				break;
			}
			if (cmp > 0) {
				first = mid+1;
				continue;
			}
			last = mid;
		}
		if (first < num) {
			unsigned char now[20], next[20];
			nth_packed_object_sha1(p, first, now);
			if (match_sha(len, match, now)) {
				if (nth_packed_object_sha1(p, first+1, next) ||
				    !match_sha(len, match, next)) {
					/* unique within this pack */
					if (!found) {
						memcpy(found_sha1, now, 20);
						found++;
					}
					else if (memcmp(found_sha1, now, 20)) {
						found = 2;
						break;
					}
				}
				else {
					/* not even unique within this pack */
					found = 2;
					break;
				}
			}
		}
	}
	if (found == 1)
		memcpy(sha1, found_sha1, 20);
	return found;
}

#define SHORT_NAME_NOT_FOUND (-1)
#define SHORT_NAME_AMBIGUOUS (-2)

static int find_unique_short_object(int len, char *canonical,
				    unsigned char *res, unsigned char *sha1)
{
	int has_unpacked, has_packed;
	unsigned char unpacked_sha1[20], packed_sha1[20];

	has_unpacked = find_short_object_filename(len, canonical, unpacked_sha1);
	has_packed = find_short_packed_object(len, res, packed_sha1);
	if (!has_unpacked && !has_packed)
		return SHORT_NAME_NOT_FOUND;
	if (1 < has_unpacked || 1 < has_packed)
		return SHORT_NAME_AMBIGUOUS;
	if (has_unpacked != has_packed) {
		memcpy(sha1, (has_packed ? packed_sha1 : unpacked_sha1), 20);
		return 0;
	}
	/* Both have unique ones -- do they match? */
	if (memcmp(packed_sha1, unpacked_sha1, 20))
		return -2;
	memcpy(sha1, packed_sha1, 20);
	return 0;
}

static int get_short_sha1(const char *name, int len, unsigned char *sha1,
			  int quietly)
{
	int i, status;
	char canonical[40];
	unsigned char res[20];

	if (len < 4)
		return -1;
	memset(res, 0, 20);
	memset(canonical, 'x', 40);
	for (i = 0; i < len ;i++) {
		unsigned char c = name[i];
		unsigned char val;
		if (c >= '0' && c <= '9')
			val = c - '0';
		else if (c >= 'a' && c <= 'f')
			val = c - 'a' + 10;
		else if (c >= 'A' && c <='F') {
			val = c - 'A' + 10;
			c -= 'A' - 'a';
		}
		else
			return -1;
		canonical[i] = c;
		if (!(i & 1))
			val <<= 4;
		res[i >> 1] |= val;
	}

	status = find_unique_short_object(i, canonical, res, sha1);
	if (!quietly && (status == SHORT_NAME_AMBIGUOUS))
		return error("short SHA1 %.*s is ambiguous.", len, canonical);
	return status;
}

const char *find_unique_abbrev(const unsigned char *sha1, int len)
{
	int status;
	static char hex[41];

	memcpy(hex, sha1_to_hex(sha1), 40);
	if (len == 40)
		return hex;
	while (len < 40) {
		unsigned char sha1_ret[20];
		status = get_short_sha1(hex, len, sha1_ret, 1);
		if (!status) {
			hex[len] = 0;
			return hex;
		}
		if (status != SHORT_NAME_AMBIGUOUS)
			return NULL;
		len++;
	}
	return NULL;
}

static int ambiguous_path(const char *path, int len)
{
	int slash = 1;
	int cnt;

	for (cnt = 0; cnt < len; cnt++) {
		switch (*path++) {
		case '\0':
			break;
		case '/':
			if (slash)
				break;
			slash = 1;
			continue;
		case '.':
			continue;
		default:
			slash = 0;
			continue;
		}
		break;
	}
	return slash;
}

static int get_sha1_basic(const char *str, int len, unsigned char *sha1)
{
	static const char *prefix[] = {
		"",
		"refs",
		"refs/tags",
		"refs/heads",
		NULL
	};
	const char **p;

	if (len == 40 && !get_sha1_hex(str, sha1))
		return 0;

	/* Accept only unambiguous ref paths. */
	if (ambiguous_path(str, len))
		return -1;

	for (p = prefix; *p; p++) {
		char *pathname = git_path("%s/%.*s", *p, len, str);
		if (!read_ref(pathname, sha1))
			return 0;
	}
	return -1;
}

static int get_sha1_1(const char *name, int len, unsigned char *sha1);

static int get_parent(const char *name, int len,
		      unsigned char *result, int idx)
{
	unsigned char sha1[20];
	int ret = get_sha1_1(name, len, sha1);
	struct commit *commit;
	struct commit_list *p;

	if (ret)
		return ret;
	commit = lookup_commit_reference(sha1);
	if (!commit)
		return -1;
	if (parse_commit(commit))
		return -1;
	if (!idx) {
		memcpy(result, commit->object.sha1, 20);
		return 0;
	}
	p = commit->parents;
	while (p) {
		if (!--idx) {
			memcpy(result, p->item->object.sha1, 20);
			return 0;
		}
		p = p->next;
	}
	return -1;
}

static int get_nth_ancestor(const char *name, int len,
			    unsigned char *result, int generation)
{
	unsigned char sha1[20];
	int ret = get_sha1_1(name, len, sha1);
	if (ret)
		return ret;

	while (generation--) {
		struct commit *commit = lookup_commit_reference(sha1);

		if (!commit || parse_commit(commit) || !commit->parents)
			return -1;
		memcpy(sha1, commit->parents->item->object.sha1, 20);
	}
	memcpy(result, sha1, 20);
	return 0;
}

static int peel_onion(const char *name, int len, unsigned char *sha1)
{
	unsigned char outer[20];
	const char *sp;
	const char *type_string = NULL;
	struct object *o;

	/*
	 * "ref^{type}" dereferences ref repeatedly until you cannot
	 * dereference anymore, or you get an object of given type,
	 * whichever comes first.  "ref^{}" means just dereference
	 * tags until you get a non-tag.  "ref^0" is a shorthand for
	 * "ref^{commit}".  "commit^{tree}" could be used to find the
	 * top-level tree of the given commit.
	 */
	if (len < 4 || name[len-1] != '}')
		return -1;

	for (sp = name + len - 1; name <= sp; sp--) {
		int ch = *sp;
		if (ch == '{' && name < sp && sp[-1] == '^')
			break;
	}
	if (sp <= name)
		return -1;

	sp++; /* beginning of type name, or closing brace for empty */
	if (!strncmp(commit_type, sp, 6) && sp[6] == '}')
		type_string = commit_type;
	else if (!strncmp(tree_type, sp, 4) && sp[4] == '}')
		type_string = tree_type;
	else if (!strncmp(blob_type, sp, 4) && sp[4] == '}')
		type_string = blob_type;
	else if (sp[0] == '}')
		type_string = NULL;
	else
		return -1;

	if (get_sha1_1(name, sp - name - 2, outer))
		return -1;

	o = parse_object(outer);
	if (!o)
		return -1;
	if (!type_string) {
		o = deref_tag(o, name, sp - name - 2);
		if (!o || (!o->parsed && !parse_object(o->sha1)))
			return -1;
		memcpy(sha1, o->sha1, 20);
	}
	else {
		/* At this point, the syntax look correct, so
		 * if we do not get the needed object, we should
		 * barf.
		 */

		while (1) {
			if (!o || (!o->parsed && !parse_object(o->sha1)))
				return -1;
			if (o->type == type_string) {
				memcpy(sha1, o->sha1, 20);
				return 0;
			}
			if (o->type == tag_type)
				o = ((struct tag*) o)->tagged;
			else if (o->type == commit_type)
				o = &(((struct commit *) o)->tree->object);
			else
				return error("%.*s: expected %s type, but the object dereferences to %s type",
					     len, name, type_string,
					     o->type);
			if (!o->parsed)
				parse_object(o->sha1);
		}
	}
	return 0;
}

static int get_sha1_1(const char *name, int len, unsigned char *sha1)
{
	int parent, ret;
	const char *cp;

	/* foo^[0-9] or foo^ (== foo^1); we do not do more than 9 parents. */
	if (len > 2 && name[len-2] == '^' &&
	    name[len-1] >= '0' && name[len-1] <= '9') {
		parent = name[len-1] - '0';
		len -= 2;
	}
	else if (len > 1 && name[len-1] == '^') {
		parent = 1;
		len--;
	} else
		parent = -1;

	if (parent >= 0)
		return get_parent(name, len, sha1, parent);

	/* "name~3" is "name^^^",
	 * "name~12" is "name^^^^^^^^^^^^", and
	 * "name~" and "name~0" are name -- not "name^0"!
	 */
	parent = 0;
	for (cp = name + len - 1; name <= cp; cp--) {
		int ch = *cp;
		if ('0' <= ch && ch <= '9')
			continue;
		if (ch != '~')
			parent = -1;
		break;
	}
	if (!parent && *cp == '~') {
		int len1 = cp - name;
		cp++;
		while (cp < name + len)
			parent = parent * 10 + *cp++ - '0';
		return get_nth_ancestor(name, len1, sha1, parent);
	}

	ret = peel_onion(name, len, sha1);
	if (!ret)
		return 0;

	ret = get_sha1_basic(name, len, sha1);
	if (!ret)
		return 0;
	return get_short_sha1(name, len, sha1, 0);
}

/*
 * This is like "get_sha1_basic()", except it allows "sha1 expressions",
 * notably "xyz^" for "parent of xyz"
 */
int get_sha1(const char *name, unsigned char *sha1)
{
	prepare_alt_odb();
	return get_sha1_1(name, strlen(name), sha1);
}
