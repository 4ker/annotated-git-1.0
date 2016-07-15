/*
*
* Copyright 2005, Lukas Sandstrom <lukass@etek.chalmers.se>
*
* This file is licensed under the GPL v2.
*
*/

#include "cache.h"

static const char pack_redundant_usage[] =
"git-pack-redundant [ --verbose ] [ --alt-odb ] < --all | <.pack filename> ...>";

static int load_all_packs = 0, verbose = 0, alt_odb = 0;

struct llist_item {
	struct llist_item *next;
	unsigned char *sha1;
};
static struct llist {
	struct llist_item *front;
	struct llist_item *back;
	size_t size;
} *all_objects; /* all objects which must be present in local packfiles */

static struct pack_list {
	struct pack_list *next;
	struct packed_git *pack;
	struct llist *unique_objects;
	struct llist *all_objects;
} *local_packs = NULL, *altodb_packs = NULL;

struct pll {
	struct pll *next;
	struct pack_list *pl;
	size_t pl_size;
};

static struct llist_item *free_nodes = NULL;

static inline struct llist_item *llist_item_get()
{
	struct llist_item *new;
	if ( free_nodes ) {
		new = free_nodes;
		free_nodes = free_nodes->next;
	} else
		new = xmalloc(sizeof(struct llist_item));

	return new;
}

static inline void llist_item_put(struct llist_item *item)
{
	item->next = free_nodes;
	free_nodes = item;
}

static void llist_free(struct llist *list)
{
	while((list->back = list->front)) {
		list->front = list->front->next;
		llist_item_put(list->back);
	}
	free(list);
}

static inline void llist_init(struct llist **list)
{
	*list = xmalloc(sizeof(struct llist));
	(*list)->front = (*list)->back = NULL;
	(*list)->size = 0;
}

static struct llist * llist_copy(struct llist *list)
{
	struct llist *ret;
	struct llist_item *new, *old, *prev;
	
	llist_init(&ret);

	if ((ret->size = list->size) == 0)
		return ret;

	new = ret->front = llist_item_get();
	new->sha1 = list->front->sha1;

	old = list->front->next;
	while (old) {
		prev = new;
		new = llist_item_get();
		prev->next = new;
		new->sha1 = old->sha1;
		old = old->next;
	}
	new->next = NULL;
	ret->back = new;
	
	return ret;
}

static inline struct llist_item * llist_insert(struct llist *list,
					       struct llist_item *after,
					       unsigned char *sha1)
{
	struct llist_item *new = llist_item_get();
	new->sha1 = sha1;
	new->next = NULL;

	if (after != NULL) {
		new->next = after->next;
		after->next = new;
		if (after == list->back)
			list->back = new;
	} else {/* insert in front */
		if (list->size == 0)
			list->back = new;
		else
			new->next = list->front;
		list->front = new;
	}
	list->size++;
	return new;
}

static inline struct llist_item *llist_insert_back(struct llist *list, unsigned char *sha1)
{
	return llist_insert(list, list->back, sha1);
}

static inline struct llist_item *llist_insert_sorted_unique(struct llist *list, unsigned char *sha1, struct llist_item *hint)
{
	struct llist_item *prev = NULL, *l;

	l = (hint == NULL) ? list->front : hint;
	while (l) {
		int cmp = memcmp(l->sha1, sha1, 20);
		if (cmp > 0) { /* we insert before this entry */
			return llist_insert(list, prev, sha1);
		}
		if(!cmp) { /* already exists */
			return l;
		}
		prev = l;
		l = l->next;
	}
	/* insert at the end */
	return llist_insert_back(list, sha1);
}

/* returns a pointer to an item in front of sha1 */
static inline struct llist_item * llist_sorted_remove(struct llist *list, const unsigned char *sha1, struct llist_item *hint)
{
	struct llist_item *prev, *l;

redo_from_start:
	l = (hint == NULL) ? list->front : hint;
	prev = NULL;
	while (l) {
		int cmp = memcmp(l->sha1, sha1, 20);
		if (cmp > 0) /* not in list, since sorted */
			return prev;
		if(!cmp) { /* found */
			if (prev == NULL) {
				if (hint != NULL && hint != list->front) {
					/* we don't know the previous element */
					hint = NULL;
					goto redo_from_start;
				}
				list->front = l->next;
			} else
				prev->next = l->next;
			if (l == list->back)
				list->back = prev;
			llist_item_put(l);
			list->size--;
			return prev;
		}
		prev = l;
		l = l->next;
	}
	return prev;
}

/* computes A\B */
static void llist_sorted_difference_inplace(struct llist *A,
				     struct llist *B)
{
	struct llist_item *hint, *b;

	hint = NULL;
	b = B->front;

	while (b) {
		hint = llist_sorted_remove(A, b->sha1, hint);
		b = b->next;
	}
}

static inline struct pack_list * pack_list_insert(struct pack_list **pl,
					   struct pack_list *entry)
{
	struct pack_list *p = xmalloc(sizeof(struct pack_list));
	memcpy(p, entry, sizeof(struct pack_list));
	p->next = *pl;
	*pl = p;
	return p;
}

static inline size_t pack_list_size(struct pack_list *pl)
{
	size_t ret = 0;
	while(pl) {
		ret++;
		pl = pl->next;
	}
	return ret;
}

static struct pack_list * pack_list_difference(const struct pack_list *A,
					       const struct pack_list *B)
{
	struct pack_list *ret;
	const struct pack_list *pl;

	if (A == NULL)
		return NULL;

	pl = B;
	while (pl != NULL) {
		if (A->pack == pl->pack)
			return pack_list_difference(A->next, B);
		pl = pl->next;
	}
	ret = xmalloc(sizeof(struct pack_list));
	memcpy(ret, A, sizeof(struct pack_list));
	ret->next = pack_list_difference(A->next, B);
	return ret;
}

static void cmp_two_packs(struct pack_list *p1, struct pack_list *p2)
{
	int p1_off, p2_off;
	void *p1_base, *p2_base;
	struct llist_item *p1_hint = NULL, *p2_hint = NULL;
	
	p1_off = p2_off = 256 * 4 + 4;
	p1_base = (void *)p1->pack->index_base;
	p2_base = (void *)p2->pack->index_base;

	while (p1_off <= p1->pack->index_size - 3 * 20 &&
	       p2_off <= p2->pack->index_size - 3 * 20)
	{
		int cmp = memcmp(p1_base + p1_off, p2_base + p2_off, 20);
		/* cmp ~ p1 - p2 */
		if (cmp == 0) {
			p1_hint = llist_sorted_remove(p1->unique_objects,
					p1_base + p1_off, p1_hint);
			p2_hint = llist_sorted_remove(p2->unique_objects,
					p1_base + p1_off, p2_hint);
			p1_off+=24;
			p2_off+=24;
			continue;
		}
		if (cmp < 0) { /* p1 has the object, p2 doesn't */
			p1_off+=24;
		} else { /* p2 has the object, p1 doesn't */
			p2_off+=24;
		}
	}
}

static void pll_insert(struct pll **pll, struct pll **hint_table)
{
	struct pll *prev;
	int i = (*pll)->pl_size - 1;

	if (hint_table[i] == NULL) {
		hint_table[i--] = *pll;
		for (; i >= 0; --i) {
			if (hint_table[i] != NULL)
				break;
		}
		if (hint_table[i] == NULL) /* no elements in list */
			die("Why did this happen?");
	}

	prev = hint_table[i];
	while (prev->next && prev->next->pl_size < (*pll)->pl_size)
		prev = prev->next;

	(*pll)->next = prev->next;
	prev->next = *pll;
}

/* all the permutations have to be free()d at the same time,
 * since they refer to each other
 */
static struct pll * get_all_permutations(struct pack_list *list)
{
	struct pll *subset, *pll, *new_pll = NULL; /*silence warning*/
	static struct pll **hint = NULL;
	if (hint == NULL)
		hint = xcalloc(pack_list_size(list), sizeof(struct pll *));
		
	if (list == NULL)
		return NULL;

	if (list->next == NULL) {
		new_pll = xmalloc(sizeof(struct pll));
		hint[0] = new_pll;
		new_pll->next = NULL;
		new_pll->pl = list;
		new_pll->pl_size = 1;
		return new_pll;
	}

	pll = subset = get_all_permutations(list->next);
	while (pll) {
		if (pll->pl->pack == list->pack) {
			pll = pll->next;
			continue;
		}
		new_pll = xmalloc(sizeof(struct pll));

		new_pll->pl = xmalloc(sizeof(struct pack_list));
		memcpy(new_pll->pl, list, sizeof(struct pack_list));
		new_pll->pl->next = pll->pl;
		new_pll->pl_size = pll->pl_size + 1;
		
		pll_insert(&new_pll, hint);

		pll = pll->next;
	}
	/* add ourself */
	new_pll = xmalloc(sizeof(struct pll));
	new_pll->pl = xmalloc(sizeof(struct pack_list));
	memcpy(new_pll->pl, list, sizeof(struct pack_list));
	new_pll->pl->next = NULL;
	new_pll->pl_size = 1;
	pll_insert(&new_pll, hint);

	return hint[0];
}

static int is_superset(struct pack_list *pl, struct llist *list)
{
	struct llist *diff;

	diff = llist_copy(list);

	while (pl) {
		llist_sorted_difference_inplace(diff, pl->all_objects);
		if (diff->size == 0) { /* we're done */
			llist_free(diff);
			return 1;
		}
		pl = pl->next;
	}
	llist_free(diff);
	return 0;
}

static size_t sizeof_union(struct packed_git *p1, struct packed_git *p2)
{
	size_t ret = 0;
	int p1_off, p2_off;
	void *p1_base, *p2_base;

	p1_off = p2_off = 256 * 4 + 4;
	p1_base = (void *)p1->index_base;
	p2_base = (void *)p2->index_base;

	while (p1_off <= p1->index_size - 3 * 20 &&
	       p2_off <= p2->index_size - 3 * 20)
	{
		int cmp = memcmp(p1_base + p1_off, p2_base + p2_off, 20);
		/* cmp ~ p1 - p2 */
		if (cmp == 0) {
			ret++;
			p1_off+=24;
			p2_off+=24;
			continue;
		}
		if (cmp < 0) { /* p1 has the object, p2 doesn't */
			p1_off+=24;
		} else { /* p2 has the object, p1 doesn't */
			p2_off+=24;
		}
	}
	return ret;
}

/* another O(n^2) function ... */
static size_t get_pack_redundancy(struct pack_list *pl)
{
	struct pack_list *subset;
	size_t ret = 0;

	if (pl == NULL)
		return 0;

	while ((subset = pl->next)) {
		while(subset) {
			ret += sizeof_union(pl->pack, subset->pack);
			subset = subset->next;
		}
		pl = pl->next;
	}
	return ret;
}

static inline size_t pack_set_bytecount(struct pack_list *pl)
{
	size_t ret = 0;
	while (pl) {
		ret += pl->pack->pack_size;
		ret += pl->pack->index_size;
		pl = pl->next;
	}
	return ret;
}

static void minimize(struct pack_list **min)
{
	struct pack_list *pl, *unique = NULL,
		*non_unique = NULL, *min_perm = NULL;
	struct pll *perm, *perm_all, *perm_ok = NULL, *new_perm;
	struct llist *missing;
	size_t min_perm_size = (size_t)-1, perm_size;

	pl = local_packs;
	while (pl) {
		if(pl->unique_objects->size)
			pack_list_insert(&unique, pl);
		else
			pack_list_insert(&non_unique, pl);
		pl = pl->next;
	}
	/* find out which objects are missing from the set of unique packs */
	missing = llist_copy(all_objects);
	pl = unique;
	while (pl) {
		llist_sorted_difference_inplace(missing,
						pl->all_objects);
		pl = pl->next;
	}

	/* return if there are no objects missing from the unique set */
	if (missing->size == 0) {
		*min = unique;
		return;
	}

	/* find the permutations which contain all missing objects */
	perm_all = perm = get_all_permutations(non_unique);
	while (perm) {
		if (perm_ok && perm->pl_size > perm_ok->pl_size)
			break; /* ignore all larger permutations */
		if (is_superset(perm->pl, missing)) {
			new_perm = xmalloc(sizeof(struct pll));
			memcpy(new_perm, perm, sizeof(struct pll));
			new_perm->next = perm_ok;
			perm_ok = new_perm;
		}
		perm = perm->next;
	}
	
	if (perm_ok == NULL)
		die("Internal error: No complete sets found!\n");

	/* find the permutation with the smallest size */
	perm = perm_ok;
	while (perm) {
		perm_size = pack_set_bytecount(perm->pl);
		if (min_perm_size > perm_size) {
			min_perm_size = perm_size;
			min_perm = perm->pl;
		}
		perm = perm->next;
	}
	*min = min_perm;
	/* add the unique packs to the list */
	pl = unique;
	while(pl) {
		pack_list_insert(min, pl);
		pl = pl->next;
	}
}

static void load_all_objects(void)
{
	struct pack_list *pl = local_packs;
	struct llist_item *hint, *l;

	llist_init(&all_objects);

	while (pl) {
		hint = NULL;
		l = pl->all_objects->front;
		while (l) {
			hint = llist_insert_sorted_unique(all_objects,
							  l->sha1, hint);
			l = l->next;
		}
		pl = pl->next;
	}
	/* remove objects present in remote packs */
	pl = altodb_packs;
	while (pl) {
		llist_sorted_difference_inplace(all_objects, pl->all_objects);
		pl = pl->next;
	}
}

/* this scales like O(n^2) */
static void cmp_local_packs(void)
{
	struct pack_list *subset, *pl = local_packs;

	while ((subset = pl)) {
		while((subset = subset->next))
			cmp_two_packs(pl, subset);
		pl = pl->next;
	}
}

static void scan_alt_odb_packs(void)
{
	struct pack_list *local, *alt;

	alt = altodb_packs;
	while (alt) {
		local = local_packs;
		while (local) {
			llist_sorted_difference_inplace(local->unique_objects,
							alt->all_objects);
			local = local->next;
		}
		alt = alt->next;
	}
}

static struct pack_list * add_pack(struct packed_git *p)
{
	struct pack_list l;
	size_t off;
	void *base;

	if (!p->pack_local && !(alt_odb || verbose))
		return NULL;

	l.pack = p;
	llist_init(&l.all_objects);

	off = 256 * 4 + 4;
	base = (void *)p->index_base;
	while (off <= p->index_size - 3 * 20) {
		llist_insert_back(l.all_objects, base + off);
		off += 24;
	}
	/* this list will be pruned in cmp_two_packs later */
	l.unique_objects = llist_copy(l.all_objects);
	if (p->pack_local)
		return pack_list_insert(&local_packs, &l);
	else
		return pack_list_insert(&altodb_packs, &l);
}

static struct pack_list * add_pack_file(char *filename)
{
	struct packed_git *p = packed_git;

	if (strlen(filename) < 40)
		die("Bad pack filename: %s\n", filename);

	while (p) {
		if (strstr(p->pack_name, filename))
			return add_pack(p);
		p = p->next;
	}
	die("Filename %s not found in packed_git\n", filename);
}

static void load_all(void)
{
	struct packed_git *p = packed_git;

	while (p) {
		add_pack(p);
		p = p->next;
	}
}

int main(int argc, char **argv)
{
	int i;
	struct pack_list *min, *red, *pl;
	struct llist *ignore;
	unsigned char *sha1;
	char buf[42]; /* 40 byte sha1 + \n + \0 */

	setup_git_directory();

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if(!strcmp(arg, "--")) {
			i++;
			break;
		}
		if(!strcmp(arg, "--all")) {
			load_all_packs = 1;
			continue;
		}
		if(!strcmp(arg, "--verbose")) {
			verbose = 1;
			continue;
		}
		if(!strcmp(arg, "--alt-odb")) {
			alt_odb = 1;
			continue;
		}
		if(*arg == '-')
			usage(pack_redundant_usage);
		else
			break;
	}

	prepare_packed_git();

	if (load_all_packs)
		load_all();
	else
		while (*(argv + i) != NULL)
			add_pack_file(*(argv + i++));

	if (local_packs == NULL)
		die("Zero packs found!\n");

	load_all_objects();

	cmp_local_packs();
	if (alt_odb)
		scan_alt_odb_packs();

	/* ignore objects given on stdin */
	llist_init(&ignore);
	if (!isatty(0)) {
		while (fgets(buf, sizeof(buf), stdin)) {
			sha1 = xmalloc(20);
			if (get_sha1_hex(buf, sha1))
				die("Bad sha1 on stdin: %s", buf);
			llist_insert_sorted_unique(ignore, sha1, NULL);
		}
	}
	llist_sorted_difference_inplace(all_objects, ignore);
	pl = local_packs;
	while (pl) {
		llist_sorted_difference_inplace(pl->unique_objects, ignore);
		pl = pl->next;
	}

	minimize(&min);

	if (verbose) {
		fprintf(stderr, "There are %lu packs available in alt-odbs.\n",
			(unsigned long)pack_list_size(altodb_packs));
		fprintf(stderr, "The smallest (bytewise) set of packs is:\n");
		pl = min;
		while (pl) {
			fprintf(stderr, "\t%s\n", pl->pack->pack_name);
			pl = pl->next;
		}
		fprintf(stderr, "containing %lu duplicate objects "
				"with a total size of %lukb.\n",
			(unsigned long)get_pack_redundancy(min),
			(unsigned long)pack_set_bytecount(min)/1024);
		fprintf(stderr, "A total of %lu unique objects were considered.\n",
			(unsigned long)all_objects->size);
		fprintf(stderr, "Redundant packs (with indexes):\n");
	}
	pl = red = pack_list_difference(local_packs, min);
	while (pl) {
		printf("%s\n%s\n",
		       sha1_pack_index_name(pl->pack->sha1),
		       pl->pack->pack_name);
		pl = pl->next;
	}
	if (verbose)
		fprintf(stderr, "%luMB of redundant packs in total.\n",
			(unsigned long)pack_set_bytecount(red)/(1024*1024));

	return 0;
}
