#include "object.h"
#include "blob.h"
#include "tree.h"
#include "commit.h"
#include "cache.h"
#include "tag.h"

struct object **objs;
int nr_objs;
static int obj_allocs;

int track_object_refs = 1;

static int find_object(const unsigned char *sha1)
{
	int first = 0, last = nr_objs;

        while (first < last) {
                int next = (first + last) / 2;
                struct object *obj = objs[next];
                int cmp;

                cmp = memcmp(sha1, obj->sha1, 20);
                if (!cmp)
                        return next;
                if (cmp < 0) {
                        last = next;
                        continue;
                }
                first = next+1;
        }
        return -first-1;
}

struct object *lookup_object(const unsigned char *sha1)
{
	int pos = find_object(sha1);
	if (pos >= 0)
		return objs[pos];
	return NULL;
}

void created_object(const unsigned char *sha1, struct object *obj)
{
	int pos = find_object(sha1);

	obj->parsed = 0;
	memcpy(obj->sha1, sha1, 20);
	obj->type = NULL;
	obj->refs = NULL;
	obj->used = 0;

	if (pos >= 0)
		die("Inserting %s twice\n", sha1_to_hex(sha1));
	pos = -pos-1;

	if (obj_allocs == nr_objs) {
		obj_allocs = alloc_nr(obj_allocs);
		objs = xrealloc(objs, obj_allocs * sizeof(struct object *));
	}

	/* Insert it into the right place */
	memmove(objs + pos + 1, objs + pos, (nr_objs - pos) * 
		sizeof(struct object *));

	objs[pos] = obj;
	nr_objs++;
}

struct object_refs *alloc_object_refs(unsigned count)
{
	struct object_refs *refs;
	size_t size = sizeof(*refs) + count*sizeof(struct object *);

	refs = xmalloc(size);
	memset(refs, 0, size);
	refs->count = count;
	return refs;
}

static int compare_object_pointers(const void *a, const void *b)
{
	const struct object * const *pa = a;
	const struct object * const *pb = b;
	if (*pa == *pb)
		return 0;
	else if (*pa < *pb)
		return -1;
	else
		return 1;
}

void set_object_refs(struct object *obj, struct object_refs *refs)
{
	unsigned int i, j;

	/* Do not install empty list of references */
	if (refs->count < 1) {
		free(refs);
		return;
	}

	/* Sort the list and filter out duplicates */
	qsort(refs->ref, refs->count, sizeof(refs->ref[0]),
	      compare_object_pointers);
	for (i = j = 1; i < refs->count; i++) {
		if (refs->ref[i] != refs->ref[i - 1])
			refs->ref[j++] = refs->ref[i];
	}
	if (j < refs->count) {
		/* Duplicates were found - reallocate list */
		size_t size = sizeof(*refs) + j*sizeof(struct object *);
		refs->count = j;
		refs = xrealloc(refs, size);
	}

	for (i = 0; i < refs->count; i++)
		refs->ref[i]->used = 1;
	obj->refs = refs;
}

void mark_reachable(struct object *obj, unsigned int mask)
{
	if (!track_object_refs)
		die("cannot do reachability with object refs turned off");
	/* If we've been here already, don't bother */
	if (obj->flags & mask)
		return;
	obj->flags |= mask;
	if (obj->refs) {
		const struct object_refs *refs = obj->refs;
		unsigned i;
		for (i = 0; i < refs->count; i++)
			mark_reachable(refs->ref[i], mask);
	}
}

struct object *lookup_object_type(const unsigned char *sha1, const char *type)
{
	if (!type) {
		return lookup_unknown_object(sha1);
	} else if (!strcmp(type, blob_type)) {
		return &lookup_blob(sha1)->object;
	} else if (!strcmp(type, tree_type)) {
		return &lookup_tree(sha1)->object;
	} else if (!strcmp(type, commit_type)) {
		return &lookup_commit(sha1)->object;
	} else if (!strcmp(type, tag_type)) {
		return &lookup_tag(sha1)->object;
	} else {
		error("Unknown type %s", type);
		return NULL;
	}
}

union any_object {
	struct object object;
	struct commit commit;
	struct tree tree;
	struct blob blob;
	struct tag tag;
};

struct object *lookup_unknown_object(const unsigned char *sha1)
{
	struct object *obj = lookup_object(sha1);
	if (!obj) {
		union any_object *ret = xmalloc(sizeof(*ret));
		memset(ret, 0, sizeof(*ret));
		created_object(sha1, &ret->object);
		ret->object.type = NULL;
		return &ret->object;
	}
	return obj;
}

struct object *parse_object(const unsigned char *sha1)
{
	unsigned long size;
	char type[20];
	void *buffer = read_sha1_file(sha1, type, &size);
	if (buffer) {
		struct object *obj;
		if (check_sha1_signature(sha1, buffer, size, type) < 0)
			printf("sha1 mismatch %s\n", sha1_to_hex(sha1));
		if (!strcmp(type, "blob")) {
			struct blob *blob = lookup_blob(sha1);
			parse_blob_buffer(blob, buffer, size);
			obj = &blob->object;
		} else if (!strcmp(type, "tree")) {
			struct tree *tree = lookup_tree(sha1);
			parse_tree_buffer(tree, buffer, size);
			obj = &tree->object;
		} else if (!strcmp(type, "commit")) {
			struct commit *commit = lookup_commit(sha1);
			parse_commit_buffer(commit, buffer, size);
			if (!commit->buffer) {
				commit->buffer = buffer;
				buffer = NULL;
			}
			obj = &commit->object;
		} else if (!strcmp(type, "tag")) {
			struct tag *tag = lookup_tag(sha1);
			parse_tag_buffer(tag, buffer, size);
			obj = &tag->object;
		} else {
			obj = NULL;
		}
		free(buffer);
		return obj;
	}
	return NULL;
}

struct object_list *object_list_insert(struct object *item,
				       struct object_list **list_p)
{
	struct object_list *new_list = xmalloc(sizeof(struct object_list));
        new_list->item = item;
        new_list->next = *list_p;
        *list_p = new_list;
        return new_list;
}

void object_list_append(struct object *item,
			struct object_list **list_p)
{
	while (*list_p) {
		list_p = &((*list_p)->next);
	}
	*list_p = xmalloc(sizeof(struct object_list));
	(*list_p)->next = NULL;
	(*list_p)->item = item;
}

unsigned object_list_length(struct object_list *list)
{
	unsigned ret = 0;
	while (list) {
		list = list->next;
		ret++;
	}
	return ret;
}

int object_list_contains(struct object_list *list, struct object *obj)
{
	while (list) {
		if (list->item == obj)
			return 1;
		list = list->next;
	}
	return 0;
}
