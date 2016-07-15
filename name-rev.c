#include <stdlib.h>
#include "cache.h"
#include "commit.h"
#include "tag.h"
#include "refs.h"

static const char name_rev_usage[] =
	"git-name-rev [--tags] ( --all | --stdin | commitish [commitish...] )\n";

typedef struct rev_name {
	const char *tip_name;
	int merge_traversals;
	int generation;
} rev_name;

static long cutoff = LONG_MAX;

static void name_rev(struct commit *commit,
		const char *tip_name, int merge_traversals, int generation,
		int deref)
{
	struct rev_name *name = (struct rev_name *)commit->object.util;
	struct commit_list *parents;
	int parent_number = 1;

	if (!commit->object.parsed)
		parse_commit(commit);

	if (commit->date < cutoff)
		return;

	if (deref) {
		char *new_name = xmalloc(strlen(tip_name)+3);
		strcpy(new_name, tip_name);
		strcat(new_name, "^0");
		tip_name = new_name;

		if (generation)
			die("generation: %d, but deref?", generation);
	}

	if (name == NULL) {
		name = xmalloc(sizeof(rev_name));
		commit->object.util = name;
		goto copy_data;
	} else if (name->merge_traversals > merge_traversals ||
			(name->merge_traversals == merge_traversals &&
			 name->generation > generation)) {
copy_data:
		name->tip_name = tip_name;
		name->merge_traversals = merge_traversals;
		name->generation = generation;
	} else
		return;

	for (parents = commit->parents;
			parents;
			parents = parents->next, parent_number++) {
		if (parent_number > 1) {
			char *new_name = xmalloc(strlen(tip_name)+8);

			if (generation > 0)
				sprintf(new_name, "%s~%d^%d", tip_name,
						generation, parent_number);
			else
				sprintf(new_name, "%s^%d", tip_name, parent_number);

			name_rev(parents->item, new_name,
				merge_traversals + 1 , 0, 0);
		} else {
			name_rev(parents->item, tip_name, merge_traversals,
				generation + 1, 0);
		}
	}
}

static int tags_only = 0;

static int name_ref(const char *path, const unsigned char *sha1)
{
	struct object *o = parse_object(sha1);
	int deref = 0;

	if (tags_only && strncmp(path, "refs/tags/", 10))
		return 0;

	while (o && o->type == tag_type) {
		struct tag *t = (struct tag *) o;
		if (!t->tagged)
			break; /* broken repository */
		o = parse_object(t->tagged->sha1);
		deref = 1;
	}
	if (o && o->type == commit_type) {
		struct commit *commit = (struct commit *)o;
		const char *p;

		while ((p = strchr(path, '/')))
			path = p+1;

		name_rev(commit, strdup(path), 0, 0, deref);
	}
	return 0;
}

/* returns a static buffer */
static const char* get_rev_name(struct object *o)
{
	static char buffer[1024];
	struct rev_name *n = (struct rev_name *)o->util;
	if (!n)
		return "undefined";

	if (!n->generation)
		return n->tip_name;

	snprintf(buffer, sizeof(buffer), "%s~%d", n->tip_name, n->generation);

	return buffer;
}
	
int main(int argc, char **argv)
{
	struct object_list *revs = NULL;
	struct object_list **walker = &revs;
	int as_is = 0, all = 0, transform_stdin = 0;

	setup_git_directory();

	if (argc < 2)
		usage(name_rev_usage);

	for (--argc, ++argv; argc; --argc, ++argv) {
		unsigned char sha1[20];
		struct object *o;
		struct commit *commit;

		if (!as_is && (*argv)[0] == '-') {
			if (!strcmp(*argv, "--")) {
				as_is = 1;
				continue;
			} else if (!strcmp(*argv, "--tags")) {
				tags_only = 1;
				continue;
			} else if (!strcmp(*argv, "--all")) {
				if (argc > 1)
					die("Specify either a list, or --all, not both!");
				all = 1;
				cutoff = 0;
				continue;
			} else if (!strcmp(*argv, "--stdin")) {
				if (argc > 1)
					die("Specify either a list, or --stdin, not both!");
				transform_stdin = 1;
				cutoff = 0;
				continue;
			}
			usage(name_rev_usage);
		}

		if (get_sha1(*argv, sha1)) {
			fprintf(stderr, "Could not get sha1 for %s. Skipping.\n",
					*argv);
			continue;
		}

		o = deref_tag(parse_object(sha1), *argv, 0);
		if (!o || o->type != commit_type) {
			fprintf(stderr, "Could not get commit for %s. Skipping.\n",
					*argv);
			continue;
		}

		commit = (struct commit *)o;

		if (cutoff > commit->date)
			cutoff = commit->date;

		object_list_append((struct object *)commit, walker);
		(*walker)->name = *argv;
		walker = &((*walker)->next);
	}

	for_each_ref(name_ref);

	if (transform_stdin) {
		char buffer[2048];
		char *p, *p_start;

		while (!feof(stdin)) {
			int forty = 0;
			p = fgets(buffer, sizeof(buffer), stdin);
			if (!p)
				break;

			for (p_start = p; *p; p++) {
#define ishex(x) (isdigit((x)) || ((x) >= 'a' && (x) <= 'f'))
				if (!ishex(*p))
					forty = 0;
				else if (++forty == 40 &&
						!ishex(*(p+1))) {
					unsigned char sha1[40];
					const char *name = "undefined";
					char c = *(p+1);

					forty = 0;

					*(p+1) = 0;
					if (!get_sha1(p - 39, sha1)) {
						struct object *o =
							lookup_object(sha1);
						if (o)
							name = get_rev_name(o);
					}
					*(p+1) = c;

					if (!strcmp(name, "undefined"))
						continue;

					fwrite(p_start, p - p_start + 1, 1,
					       stdout);
					printf(" (%s)", name);
					p_start = p + 1;
				}
			}

			/* flush */
			if (p_start != p)
				fwrite(p_start, p - p_start, 1, stdout);
		}
	} else if (all) {
		int i;

		for (i = 0; i < nr_objs; i++)
			printf("%s %s\n", sha1_to_hex(objs[i]->sha1),
					get_rev_name(objs[i]));
	} else
		for ( ; revs; revs = revs->next)
			printf("%s %s\n", revs->name, get_rev_name(revs->item));

	return 0;
}

