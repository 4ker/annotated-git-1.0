#include "tag.h"
#include "commit.h"
#include "cache.h"

int save_commit_buffer = 1;

struct sort_node
{
	/*
         * the number of children of the associated commit
         * that also occur in the list being sorted.
         */
	unsigned int indegree;

	/*
         * reference to original list item that we will re-use
         * on output.
         */
	struct commit_list * list_item;

};

const char *commit_type = "commit";

enum cmit_fmt get_commit_format(const char *arg)
{
	if (!*arg)
		return CMIT_FMT_DEFAULT;
	if (!strcmp(arg, "=raw"))
		return CMIT_FMT_RAW;
	if (!strcmp(arg, "=medium"))
		return CMIT_FMT_MEDIUM;
	if (!strcmp(arg, "=short"))
		return CMIT_FMT_SHORT;
	if (!strcmp(arg, "=full"))
		return CMIT_FMT_FULL;
	if (!strcmp(arg, "=fuller"))
		return CMIT_FMT_FULLER;
	if (!strcmp(arg, "=oneline"))
		return CMIT_FMT_ONELINE;
	die("invalid --pretty format");
}

static struct commit *check_commit(struct object *obj,
				   const unsigned char *sha1,
				   int quiet)
{
	if (obj->type != commit_type) {
		if (!quiet)
			error("Object %s is a %s, not a commit",
			      sha1_to_hex(sha1), obj->type);
		return NULL;
	}
	return (struct commit *) obj;
}

struct commit *lookup_commit_reference_gently(const unsigned char *sha1,
					      int quiet)
{
	struct object *obj = deref_tag(parse_object(sha1), NULL, 0);

	if (!obj)
		return NULL;
	return check_commit(obj, sha1, quiet);
}

struct commit *lookup_commit_reference(const unsigned char *sha1)
{
	return lookup_commit_reference_gently(sha1, 0);
}

struct commit *lookup_commit(const unsigned char *sha1)
{
	struct object *obj = lookup_object(sha1);
	if (!obj) {
		struct commit *ret = xmalloc(sizeof(struct commit));
		memset(ret, 0, sizeof(struct commit));
		created_object(sha1, &ret->object);
		ret->object.type = commit_type;
		return ret;
	}
	if (!obj->type)
		obj->type = commit_type;
	return check_commit(obj, sha1, 0);
}

static unsigned long parse_commit_date(const char *buf)
{
	unsigned long date;

	if (memcmp(buf, "author", 6))
		return 0;
	while (*buf++ != '\n')
		/* nada */;
	if (memcmp(buf, "committer", 9))
		return 0;
	while (*buf++ != '>')
		/* nada */;
	date = strtoul(buf, NULL, 10);
	if (date == ULONG_MAX)
		date = 0;
	return date;
}

static struct commit_graft {
	unsigned char sha1[20];
	int nr_parent;
	unsigned char parent[0][20]; /* more */
} **commit_graft;
static int commit_graft_alloc, commit_graft_nr;

static int commit_graft_pos(const unsigned char *sha1)
{
	int lo, hi;
	lo = 0;
	hi = commit_graft_nr;
	while (lo < hi) {
		int mi = (lo + hi) / 2;
		struct commit_graft *graft = commit_graft[mi];
		int cmp = memcmp(sha1, graft->sha1, 20);
		if (!cmp)
			return mi;
		if (cmp < 0)
			hi = mi;
		else
			lo = mi + 1;
	}
	return -lo - 1;
}

static void prepare_commit_graft(void)
{
	char *graft_file = get_graft_file();
	FILE *fp = fopen(graft_file, "r");
	char buf[1024];
	if (!fp) {
		commit_graft = (struct commit_graft **) "hack";
		return;
	}
	while (fgets(buf, sizeof(buf), fp)) {
		/* The format is just "Commit Parent1 Parent2 ...\n" */
		int len = strlen(buf);
		int i;
		struct commit_graft *graft = NULL;

		if (buf[len-1] == '\n')
			buf[--len] = 0;
		if (buf[0] == '#')
			continue;
		if ((len + 1) % 41) {
		bad_graft_data:
			error("bad graft data: %s", buf);
			free(graft);
			continue;
		}
		i = (len + 1) / 41 - 1;
		graft = xmalloc(sizeof(*graft) + 20 * i);
		graft->nr_parent = i;
		if (get_sha1_hex(buf, graft->sha1))
			goto bad_graft_data;
		for (i = 40; i < len; i += 41) {
			if (buf[i] != ' ')
				goto bad_graft_data;
			if (get_sha1_hex(buf + i + 1, graft->parent[i/41]))
				goto bad_graft_data;
		}
		i = commit_graft_pos(graft->sha1);
		if (0 <= i) {
			error("duplicate graft data: %s", buf);
			free(graft);
			continue;
		}
		i = -i - 1;
		if (commit_graft_alloc <= ++commit_graft_nr) {
			commit_graft_alloc = alloc_nr(commit_graft_alloc);
			commit_graft = xrealloc(commit_graft,
						sizeof(*commit_graft) *
						commit_graft_alloc);
		}
		if (i < commit_graft_nr)
			memmove(commit_graft + i + 1,
				commit_graft + i,
				(commit_graft_nr - i - 1) *
				sizeof(*commit_graft));
		commit_graft[i] = graft;
	}
	fclose(fp);
}

static struct commit_graft *lookup_commit_graft(const unsigned char *sha1)
{
	int pos;
	if (!commit_graft)
		prepare_commit_graft();
	pos = commit_graft_pos(sha1);
	if (pos < 0)
		return NULL;
	return commit_graft[pos];
}

int parse_commit_buffer(struct commit *item, void *buffer, unsigned long size)
{
	char *bufptr = buffer;
	unsigned char parent[20];
	struct commit_list **pptr;
	struct commit_graft *graft;
	unsigned n_refs = 0;

	if (item->object.parsed)
		return 0;
	item->object.parsed = 1;
	if (memcmp(bufptr, "tree ", 5))
		return error("bogus commit object %s", sha1_to_hex(item->object.sha1));
	if (get_sha1_hex(bufptr + 5, parent) < 0)
		return error("bad tree pointer in commit %s\n", sha1_to_hex(item->object.sha1));
	item->tree = lookup_tree(parent);
	if (item->tree)
		n_refs++;
	bufptr += 46; /* "tree " + "hex sha1" + "\n" */
	pptr = &item->parents;

	graft = lookup_commit_graft(item->object.sha1);
	while (!memcmp(bufptr, "parent ", 7)) {
		struct commit *new_parent;

		if (get_sha1_hex(bufptr + 7, parent) || bufptr[47] != '\n')
			return error("bad parents in commit %s", sha1_to_hex(item->object.sha1));
		bufptr += 48;
		if (graft)
			continue;
		new_parent = lookup_commit(parent);
		if (new_parent) {
			pptr = &commit_list_insert(new_parent, pptr)->next;
			n_refs++;
		}
	}
	if (graft) {
		int i;
		struct commit *new_parent;
		for (i = 0; i < graft->nr_parent; i++) {
			new_parent = lookup_commit(graft->parent[i]);
			if (!new_parent)
				continue;
			pptr = &commit_list_insert(new_parent, pptr)->next;
			n_refs++;
		}
	}
	item->date = parse_commit_date(bufptr);

	if (track_object_refs) {
		unsigned i = 0;
		struct commit_list *p;
		struct object_refs *refs = alloc_object_refs(n_refs);
		if (item->tree)
			refs->ref[i++] = &item->tree->object;
		for (p = item->parents; p; p = p->next)
			refs->ref[i++] = &p->item->object;
		set_object_refs(&item->object, refs);
	}

	return 0;
}

int parse_commit(struct commit *item)
{
	char type[20];
	void *buffer;
	unsigned long size;
	int ret;

	if (item->object.parsed)
		return 0;
	buffer = read_sha1_file(item->object.sha1, type, &size);
	if (!buffer)
		return error("Could not read %s",
			     sha1_to_hex(item->object.sha1));
	if (strcmp(type, commit_type)) {
		free(buffer);
		return error("Object %s not a commit",
			     sha1_to_hex(item->object.sha1));
	}
	ret = parse_commit_buffer(item, buffer, size);
	if (save_commit_buffer && !ret) {
		item->buffer = buffer;
		return 0;
	}
	free(buffer);
	return ret;
}

struct commit_list *commit_list_insert(struct commit *item, struct commit_list **list_p)
{
	struct commit_list *new_list = xmalloc(sizeof(struct commit_list));
	new_list->item = item;
	new_list->next = *list_p;
	*list_p = new_list;
	return new_list;
}

void free_commit_list(struct commit_list *list)
{
	while (list) {
		struct commit_list *temp = list;
		list = temp->next;
		free(temp);
	}
}

struct commit_list * insert_by_date(struct commit *item, struct commit_list **list)
{
	struct commit_list **pp = list;
	struct commit_list *p;
	while ((p = *pp) != NULL) {
		if (p->item->date < item->date) {
			break;
		}
		pp = &p->next;
	}
	return commit_list_insert(item, pp);
}

	
void sort_by_date(struct commit_list **list)
{
	struct commit_list *ret = NULL;
	while (*list) {
		insert_by_date((*list)->item, &ret);
		*list = (*list)->next;
	}
	*list = ret;
}

struct commit *pop_most_recent_commit(struct commit_list **list,
				      unsigned int mark)
{
	struct commit *ret = (*list)->item;
	struct commit_list *parents = ret->parents;
	struct commit_list *old = *list;

	*list = (*list)->next;
	free(old);

	while (parents) {
		struct commit *commit = parents->item;
		parse_commit(commit);
		if (!(commit->object.flags & mark)) {
			commit->object.flags |= mark;
			insert_by_date(commit, list);
		}
		parents = parents->next;
	}
	return ret;
}

/*
 * Generic support for pretty-printing the header
 */
static int get_one_line(const char *msg, unsigned long len)
{
	int ret = 0;

	while (len--) {
		char c = *msg++;
		ret++;
		if (c == '\n')
			break;
		if (!c)
			return 0;
	}
	return ret;
}

static int add_user_info(const char *what, enum cmit_fmt fmt, char *buf, const char *line)
{
	char *date;
	int namelen;
	unsigned long time;
	int tz, ret;
	const char *filler = "    ";

	if (fmt == CMIT_FMT_ONELINE)
		return 0;
	date = strchr(line, '>');
	if (!date)
		return 0;
	namelen = ++date - line;
	time = strtoul(date, &date, 10);
	tz = strtol(date, NULL, 10);

	ret = sprintf(buf, "%s: %.*s%.*s\n", what,
		      (fmt == CMIT_FMT_FULLER) ? 4 : 0,
		      filler, namelen, line);
	switch (fmt) {
	case CMIT_FMT_MEDIUM:
		ret += sprintf(buf + ret, "Date:   %s\n", show_date(time, tz));
		break;
	case CMIT_FMT_FULLER:
		ret += sprintf(buf + ret, "%sDate: %s\n", what, show_date(time, tz));
		break;
	default:
		/* notin' */
		break;
	}
	return ret;
}

static int is_empty_line(const char *line, int len)
{
	while (len && isspace(line[len-1]))
		len--;
	return !len;
}

static int add_parent_info(enum cmit_fmt fmt, char *buf, const char *line, int parents)
{
	int offset = 0;

	if (fmt == CMIT_FMT_ONELINE)
		return offset;
	switch (parents) {
	case 1:
		break;
	case 2:
		/* Go back to the previous line: 40 characters of previous parent, and one '\n' */
		offset = sprintf(buf, "Merge: %.40s\n", line-41);
		/* Fallthrough */
	default:
		/* Replace the previous '\n' with a space */
		buf[offset-1] = ' ';
		offset += sprintf(buf + offset, "%.40s\n", line+7);
	}
	return offset;
}

unsigned long pretty_print_commit(enum cmit_fmt fmt, const char *msg, unsigned long len, char *buf, unsigned long space)
{
	int hdr = 1, body = 0;
	unsigned long offset = 0;
	int parents = 0;
	int indent = (fmt == CMIT_FMT_ONELINE) ? 0 : 4;

	for (;;) {
		const char *line = msg;
		int linelen = get_one_line(msg, len);

		if (!linelen)
			break;

		/*
		 * We want some slop for indentation and a possible
		 * final "...". Thus the "+ 20".
		 */
		if (offset + linelen + 20 > space) {
			memcpy(buf + offset, "    ...\n", 8);
			offset += 8;
			break;
		}

		msg += linelen;
		len -= linelen;
		if (hdr) {
			if (linelen == 1) {
				hdr = 0;
				if (fmt != CMIT_FMT_ONELINE)
					buf[offset++] = '\n';
				continue;
			}
			if (fmt == CMIT_FMT_RAW) {
				memcpy(buf + offset, line, linelen);
				offset += linelen;
				continue;
			}
			if (!memcmp(line, "parent ", 7)) {
				if (linelen != 48)
					die("bad parent line in commit");
				offset += add_parent_info(fmt, buf + offset, line, ++parents);
			}

			/*
			 * MEDIUM == DEFAULT shows only author with dates.
			 * FULL shows both authors but not dates.
			 * FULLER shows both authors and dates.
			 */
			if (!memcmp(line, "author ", 7))
				offset += add_user_info("Author", fmt,
							buf + offset,
							line + 7);
			if (!memcmp(line, "committer ", 10) &&
			    (fmt == CMIT_FMT_FULL || fmt == CMIT_FMT_FULLER))
				offset += add_user_info("Commit", fmt,
							buf + offset,
							line + 10);
			continue;
		}

		if (is_empty_line(line, linelen)) {
			if (!body)
				continue;
			if (fmt == CMIT_FMT_SHORT)
				break;
		} else {
			body = 1;
		}

		memset(buf + offset, ' ', indent);
		memcpy(buf + offset + indent, line, linelen);
		offset += linelen + indent;
		if (fmt == CMIT_FMT_ONELINE)
			break;
	}
	if (fmt == CMIT_FMT_ONELINE) {
		/* We do not want the terminating newline */
		if (buf[offset - 1] == '\n')
			offset--;
	}
	else {
		/* Make sure there is an EOLN */
		if (buf[offset - 1] != '\n')
			buf[offset++] = '\n';
	}
	buf[offset] = '\0';
	return offset;
}

struct commit *pop_commit(struct commit_list **stack)
{
	struct commit_list *top = *stack;
	struct commit *item = top ? top->item : NULL;

	if (top) {
		*stack = top->next;
		free(top);
	}
	return item;
}

int count_parents(struct commit * commit)
{
        int count = 0;
        struct commit_list * parents = commit->parents;
        for (count=0;parents; parents=parents->next,count++)
          ;
        return count;
}

/*
 * Performs an in-place topological sort on the list supplied.
 */
void sort_in_topological_order(struct commit_list ** list)
{
	struct commit_list * next = *list;
	struct commit_list * work = NULL, **insert;
	struct commit_list ** pptr = list;
	struct sort_node * nodes;
	struct sort_node * next_nodes;
	int count = 0;

	/* determine the size of the list */
	while (next) {
		next = next->next;
		count++;
	}
	/* allocate an array to help sort the list */
	nodes = xcalloc(count, sizeof(*nodes));
	/* link the list to the array */
	next_nodes = nodes;
	next=*list;
	while (next) {
		next_nodes->list_item = next;
		next->item->object.util = next_nodes;
		next_nodes++;
		next = next->next;
	}
	/* update the indegree */
	next=*list;
	while (next) {
		struct commit_list * parents = next->item->parents;
		while (parents) {
			struct commit * parent=parents->item;
			struct sort_node * pn = (struct sort_node *)parent->object.util;
			
			if (pn)
				pn->indegree++;
			parents=parents->next;
		}
		next=next->next;
	}
	/* 
         * find the tips
         *
         * tips are nodes not reachable from any other node in the list 
         * 
         * the tips serve as a starting set for the work queue.
         */
	next=*list;
	insert = &work;
	while (next) {
		struct sort_node * node = (struct sort_node *)next->item->object.util;

		if (node->indegree == 0) {
			insert = &commit_list_insert(next->item, insert)->next;
		}
		next=next->next;
	}
	/* process the list in topological order */
	while (work) {
		struct commit * work_item = pop_commit(&work);
		struct sort_node * work_node = (struct sort_node *)work_item->object.util;
		struct commit_list * parents = work_item->parents;

		while (parents) {
			struct commit * parent=parents->item;
			struct sort_node * pn = (struct sort_node *)parent->object.util;
			
			if (pn) {
				/* 
				 * parents are only enqueued for emission 
                                 * when all their children have been emitted thereby
                                 * guaranteeing topological order.
                                 */
				pn->indegree--;
				if (!pn->indegree) 
					commit_list_insert(parent, &work);
			}
			parents=parents->next;
		}
		/*
                 * work_item is a commit all of whose children
                 * have already been emitted. we can emit it now.
                 */
		*pptr = work_node->list_item;
		pptr = &(*pptr)->next;
		*pptr = NULL;
		work_item->object.util = NULL;
	}
	free(nodes);
}
