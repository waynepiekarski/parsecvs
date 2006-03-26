/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#include "cvs.h"

/*
 * Add head or tag refs
 */

rev_ref *
rev_ref_add (rev_ref **list, rev_commit *commit, char *name, int degree, int head)
{
    rev_ref	*r;

    while (*list)
	list = &(*list)->next;
    r = calloc (1, sizeof (rev_ref));
    r->commit = commit;
    r->name = name;
    r->next = *list;
    r->degree = degree;
    r->head = head;
    *list = r;
    return r;
}

rev_ref *
rev_list_add_head (rev_list *rl, rev_commit *commit, char *name, int degree)
{
    return rev_ref_add (&rl->heads, commit, name, degree, 1);
}

rev_ref *
rev_list_add_tag (rev_list *rl, rev_commit *commit, char *name, int degree)
{
    return rev_ref_add (&rl->tags, commit, name, degree, 0);
}

static rev_ref *
rev_find_head (rev_list *rl, char *name)
{
    rev_ref	*h;

    for (h = rl->heads; h; h = h->next)
	if (h->name == name)
	    return h;
    return NULL;
}

static rev_ref *
rev_find_tag (rev_list *rl, char *name)
{
    rev_ref	*t;

    for (t = rl->tags; t; t = t->next)
	if (t->name == name)
	    return t;
    return NULL;
}

/*
 * We keep all file lists in a canonical sorted order,
 * first by latest date and then by the address of the rev_file object
 * (which are always unique)
 */

int
rev_file_later (rev_file *af, rev_file *bf)
{
    long	t;

    /*
     * When merging file lists, we should never see the same
     * object in both trees
     */
    assert (af != bf);

    t = time_compare (af->date, bf->date);

    if (t > 0)
	return 1;
    if (t < 0)
	return 0;
    if ((uintptr_t) af > (uintptr_t) bf)
	return 1;
    return 0;
}

int
rev_commit_later (rev_commit *a, rev_commit *b)
{
    long	t;

    assert (a != b);
    t = time_compare (a->date, b->date);
    if (t > 0)
	return 1;
    if (t < 0)
	return 0;
    if ((uintptr_t) a > (uintptr_t) b)
	return 1;
    return 0;
}

#if 0
static rev_ref *
rev_find_ref (rev_list *rl, char *name)
{
    rev_ref	*r;

    r = rev_find_head (rl, name);
    if (!r)
	r = rev_find_tag (rl, name);
    return r;
}
#endif

/*
 * Commits further than 60 minutes apart are assume to be different
 */
static int
commit_time_close (time_t a, time_t b)
{
    long	diff = a - b;
    if (diff < 0) diff = -diff;
    if (diff < 60 * 60)
	return 1;
    return 0;
}

/*
 * The heart of the merge operation; detect when two
 * commits are "the same"
 */
static int
rev_commit_match (rev_commit *a, rev_commit *b)
{
    /*
     * Very recent versions of CVS place a commitid in
     * each commit to track patch sets. Use it if present
     */
    if (a->commitid && b->commitid)
	return a->commitid == b->commitid;
    if (a->commitid || b->commitid)
	return 0;
    if (!commit_time_close (a->date, b->date))
	return 0;
    if (a->log != b->log)
	return 0;
    return 1;
}

static void
rev_commit_dump (FILE *f, char *title, rev_commit *c, rev_commit *m)
{
    fprintf (f, "\n%s\n", title);
    while (c) {
	int	i;

	fprintf (f, "%c0x%x %s\n", c == m ? '>' : ' ',
		(int) c, ctime_nonl (&c->date));
	for (i = 0; i < c->nfiles; i++) {
	    fprintf (f, "\t%s", ctime_nonl (&c->files[i]->date));
	    dump_number_file (f, c->files[i]->name, &c->files[i]->number);
	    fprintf (f, "\n");
	}
	fprintf (f, "\n");
	c = c->parent;
    }
}

void
rev_list_set_tail (rev_list *rl)
{
    rev_ref	*head, *tag;
    rev_commit	*c;
    int		tail;

    for (head = rl->heads; head; head = head->next) {
	tail = 1;
	if (head->commit && head->commit->seen) {
	    head->tail = tail;
	    tail = 0;
	}
	for (c = head->commit; c; c = c->parent) {
	    if (tail && c->parent && c->seen < c->parent->seen) {
		c->tail = 1;
		tail = 0;
	    }
	    c->seen++;
	}
	head->commit->tagged = 1;
    }
    for (tag = rl->tags; tag; tag = tag->next)
	tag->commit->tagged = 1;
}

static int
rev_ref_len (rev_ref *r)
{
    int	l = 0;
    while (r) {
	l++;
	r = r->next;
    }
    return l;
}

static rev_ref *
rev_ref_sel (rev_ref *r, int len)
{
    rev_ref	*head, **tail;
    rev_ref	*a = r;
    rev_ref	*b;
    int		alen = len / 2;
    int		blen = len - alen;
    int		i;

    if (len <= 1)
	return r;

    /*
     * split
     */
    for (i = 0; i < alen - 1; i++)
	r = r->next;
    b = r->next;
    r->next = 0;
    /*
     * recurse
     */
    a = rev_ref_sel (a, alen);
    b = rev_ref_sel (b, blen);
    /*
     * merge
     */
    tail = &head;
    while (a && b) {
	if (a->degree < b->degree) {
	    *tail = a;
	    a = a->next;
	} else {
	    *tail = b;
	    b = b->next;
	}
	tail = &(*tail)->next;
    }
    /*
     * paste
     */
    if (a)
	*tail = a;
    else
	*tail = b;
    /*
     * done
     */
    return head;
}

static rev_ref *
rev_ref_sel_sort (rev_ref *r)
{
    rev_ref	*s;

    r = rev_ref_sel (r, rev_ref_len (r));
    for (s = r; s && s->next; s = s->next) {
	assert (s->degree <= s->next->degree);
    }
    return r;
}

static int
rev_list_count (rev_list *head)
{
    int	count = 0;
    while (head) {
	count++;
	head = head->next;
    }
    return count;
}

static int
rev_commit_date_compare (const void *av, const void *bv)
{
    const rev_commit	*a = *(const rev_commit **) av;
    const rev_commit	*b = *(const rev_commit **) bv;
    int			t;

    /*
     * NULL entries sort last
     */
    if (!a && !b)
	return 0;
    else if (!a)
	return 1;
    else if (!b)
	return -1;
#if 0
    /*
     * Entries with no files sort next
     */
    if (a->nfiles != b->nfiles)
	return b->nfiles - a->nfiles;
#endif
    /*
     * tailed entries sort next
     */
    if (a->tailed != b->tailed)
	return a->tailed - b->tailed;
    /*
     * Newest entries sort first
     */
    t = -time_compare (a->date, b->date);
    if (t)
	return t;
    if (a->nfiles && b->nfiles) {
	/*
	 * Ensure total order by ordering based on file address
	 */
	if ((uintptr_t) a->files[0] > (uintptr_t) b->files[0])
	    return -1;
	if ((uintptr_t) a->files[0] < (uintptr_t) b->files[0])
	    return 1;
    }
    return 0;
}

static int
rev_commit_date_sort (rev_commit **commits, int ncommit)
{
    qsort (commits, ncommit, sizeof (rev_commit *),
	   rev_commit_date_compare);
    /*
     * Trim off NULL entries
     */
    while (ncommit && !commits[ncommit-1])
	ncommit--;
    return ncommit;
}

static int
rev_commit_has_file (rev_commit *c, rev_file *f)
{
    int	n;

    for (n = 0; n < c->nfiles; n++)
	if (c->files[n] == f)
	    return 1;
    return 0;
}

static rev_file *
rev_commit_find_file (rev_commit *c, char *name)
{
    int	n;

    for (n = 0; n < c->nfiles; n++)
	if (c->files[n]->name == name)
	    return c->files[n];
    return NULL;
}

static rev_commit *
rev_commit_build (rev_commit **commits, int ncommit)
{
    int	n, nfile;
    rev_commit	*commit;

    commit = calloc (1, sizeof (rev_commit) +
		     ncommit * sizeof (rev_file *));

    commit->date = commits[0]->date;
    commit->commitid = commits[0]->commitid;
    commit->log = commits[0]->log;
    nfile = 0;
    for (n = 0; n < ncommit; n++)
	if (commits[n]->nfiles > 0)
	    commit->files[nfile++] = commits[n]->files[0];
    commit->nfiles = nfile;
    return commit;
}

static rev_commit *
rev_ref_find_commit_file (rev_ref *branch, rev_file *file)
{
    rev_commit	*c;

    for (c = branch->commit; c; c = c->parent)
	if (rev_commit_has_file (c, file))
	    return c;
    return NULL;
}

static int
rev_commit_is_ancestor (rev_commit *old, rev_commit *young)
{
    while (young) {
	if (young == old)
	    return 1;
	young = young->parent;
    }
    return 0;
}

static rev_commit *
rev_commit_locate_date (rev_ref *branch, time_t date)
{
    rev_commit	*commit;

    for (commit = branch->commit; commit; commit = commit->parent)
    {
	if (time_compare (commit->date, date) <= 0)
	    return commit;
    }
    return NULL;
}

static rev_commit *
rev_commit_locate_one (rev_ref *branch, rev_commit *file)
{
    rev_commit	*commit;

    if (!branch)
	return NULL;

    for (commit = branch->commit;
	 commit;
	 commit = commit->parent)
    {
	if (rev_commit_match (commit, file))
	    return commit;
    }
    return NULL;
}

static rev_commit *
rev_commit_locate_any (rev_ref *branch, rev_commit *file)
{
    rev_commit	*commit;

    if (!branch)
	return NULL;
    commit = rev_commit_locate_any (branch->next, file);
    if (commit)
	return commit;
    return rev_commit_locate_one (branch, file);
}

static rev_commit *
rev_commit_locate (rev_ref *branch, rev_commit *file)
{
    rev_commit	*commit;

    /*
     * Check the presumed trunk first
     */
    commit = rev_commit_locate_one (branch, file);
    if (commit)
	return commit;
    /*
     * Now look through all branches
     */
    while (branch->parent)
	branch = branch->parent;
    return rev_commit_locate_any (branch, file);
}

rev_ref *
rev_branch_of_commit (rev_list *rl, rev_commit *commit)
{
    rev_ref	*h;
    rev_commit	*c;

    for (h = rl->heads; h; h = h->next)
    {
	if (h->tail)
	    continue;
	for (c = h->commit; c; c = c->parent) {
	    if (rev_commit_match (c, commit))
		return h;
	    if (c->tail)
		break;
	}
    }
    return NULL;
}

static void
rev_branch_merge (rev_ref **branches, int nbranch,
		  rev_ref *branch, rev_list *rl)
{
    int		nlive;
    int		n;
    rev_commit	*prev = NULL;
    rev_commit	*head = NULL, **tail = &head;
    rev_commit	**commits = calloc (nbranch, sizeof (rev_commit *));
    rev_commit	*commit;

    nlive = 0;
    for (n = 0; n < nbranch; n++) {
	/*
	 * Initialize commits to head of each branch
	 */
	commits[n] = branches[n]->commit;
	/*
	 * Compute number of branches with remaining entries
	 */
	if (branches[n]->tail) {
	    if (commits[n])
		commits[n]->tailed = 1;
	} else
	    nlive++;
    }
    /*
     * Walk down branches until each one has merged with the
     * parent branch
     */
    while (nlive > 0 && nbranch > 0) {
	nbranch = rev_commit_date_sort (commits, nbranch);

	/*
	 * Construct current commit
	 */
	commit = rev_commit_build (commits, nbranch);

	/*
	 * Step each branch
	 */
	nlive = 0;
	for (n = nbranch - 1; n >= 0; n--) {
	    if (commits[n]->tailed)
		;
	    else if (n == 0 || rev_commit_match (commits[0], commits[n])) {
		/*
		 * Check to see if this entry should keep advancing
		 * Stop when we reach the revision at the
		 * merge point or when we run out of commits on the
		 * branch
		 */
		if (commits[n]->tail) {
		    assert (commits[n]->parent);
		    commits[n]->parent->tailed = 1;
		} else if (commits[n]->parent) {
		    nlive++;
		}
		commits[n] = commits[n]->parent;
	    } else if (commits[n]->parent || commits[n]->nfiles)
		nlive++;
	}
	    
	*tail = commit;
	tail = &commit->parent;
	prev = commit;
    }
    /*
     * Connect to parent branch
     */
    nbranch = rev_commit_date_sort (commits, nbranch);
    if (nbranch && branch->parent )
    {
	rev_ref	*lost;
	int	present;

//	present = 0;
	for (present = 0; present < nbranch; present++)
	    if (commits[present]->nfiles)
		break;
	if (present == nbranch)
	    *tail = NULL;
	else if ((*tail = rev_commit_locate_one (branch->parent, commits[present]))) {
	    if (prev && time_compare ((*tail)->date, prev->date) > 0) {
		fprintf (stderr, "Warning: branch point %s -> %s later than branch\n",
			 branch->name, branch->parent->name);
#if 0
		for (n = 0; n < nbranch; n++) {
		    fprintf (stderr, "\ttrunk(%3d):  %s %s", n,
			     ctime_nonl (&commits[n]->date), commits[n]->nfiles ? " " : "D" );
//		    if (commits[n]->nfiles)
			dump_number_file (stderr,
					  commits[n]->files[0]->name,
					  &commits[n]->files[0]->number);
		    fprintf (stderr, "\n");
		}
		for (n = 0; n < prev->nfiles; n++) {
		    fprintf (stderr, "\tbranch(%3d): %s", n,
			     ctime_nonl (&prev->files[n]->date));
		    dump_number_file (stderr,
				      prev->files[n]->name,
				      &prev->files[n]->number);
		    fprintf (stderr, "\n");
		}
#endif
	    }
	} else if ((*tail = rev_commit_locate_date (branch->parent,
						  commits[present]->date)))
	    fprintf (stderr, "Warning: branch point %s -> %s matched by date\n",
		     branch->name, branch->parent->name);
	else {
	    fprintf (stderr, "Error: branch point %s -> %s not found.",
		     branch->name, branch->parent->name);

	    if ((lost = rev_branch_of_commit (rl, commits[present])))
		fprintf (stderr, " Possible match on %s.", lost->name);
	    fprintf (stderr, "\n");
	}
	if (*tail) {
	    if (prev)
		prev->tail = 1;
	} else 
	    *tail = rev_commit_build (commits, nbranch);
    }
    for (n = 0; n < nbranch; n++)
	if (commits[n])
	    commits[n]->tailed = 0;
    free (commits);
    branch->commit = head;
}

/*
 * Locate position in tree cooresponding to specific tag
 */
static void
rev_tag_search (rev_ref **tags, int ntag, rev_ref *tag, rev_list *rl)
{
    rev_commit	**commits = calloc (ntag, sizeof (rev_commit *));
    int		n;

    for (n = 0; n < ntag; n++)
	commits[n] = tags[n]->commit;
    ntag = rev_commit_date_sort (commits, ntag);
    
    tag->parent = rev_branch_of_commit (rl, commits[0]);
    if (tag->parent)
	tag->commit = rev_commit_locate (tag->parent, commits[0]);
    if (!tag->commit)
	tag->commit = rev_commit_build (commits, ntag);
    free (commits);
}

static void
rev_ref_set_parent (rev_list *rl, rev_ref *dest, rev_list *source)
{
    rev_ref	*sh;
    rev_list	*s;
    rev_ref	*p;
    rev_ref	*max;

    if (dest->depth)
	return;

    max = NULL;
    for (s = source; s; s = s->next) {
	sh = rev_find_head (s, dest->name);
	if (!sh)
	    continue;
	if (!sh->parent)
	    continue;
	p = rev_find_head (rl, sh->parent->name);
	assert (p);
	rev_ref_set_parent (rl, p, source);
	if (!max || p->depth > max->depth)
	    max = p;
    }
    dest->parent = max;
    if (max)
	dest->depth = max->depth + 1;
    else
	dest->depth = 1;
}


static void
rev_head_find_parent (rev_list *rl, rev_ref *h, rev_list *lhead)
{
    rev_list	*l;
    rev_ref	*lh;

    for (l = lhead; l; l = l->next) {
	lh = rev_find_head (l, h->name);
	if (!lh)
	    continue;
	
    }
}

static int
rev_branch_name_is_ancestor (rev_ref *old, rev_ref *young)
{
    while (young) {
	if (young->name == old->name)
	    return 1;
	young = young->parent;
    }
    return 0;
}

static rev_ref *
rev_ref_parent (rev_ref **refs, int nref, rev_list *rl)
{
    rev_ref	*parent = NULL;
    rev_ref	*branch = NULL;
    int		n;
    rev_ref	*h;

    for (n = 0; n < nref; n++)
    {
	if (refs[n]->parent) {
	    if (!parent) {
		parent = refs[n]->parent;
		branch = refs[n];
	    } else if (parent->name != refs[n]->parent->name) {
		if (rev_branch_name_is_ancestor (refs[n]->parent, parent))
		    ;
		else if (rev_branch_name_is_ancestor (parent, refs[n]->parent)) {
		    parent = refs[n]->parent;
		    branch = refs[n];
		} else {
		    fprintf (stderr, "Branch name collision:\n");
		    fprintf (stderr, "\tfirst branch: ");
		    dump_ref_name (stderr, branch);
		    fprintf (stderr, "\n");
		    fprintf (stderr, "\tsecond branch: ");
		    dump_ref_name (stderr, refs[n]);
		    fprintf (stderr, "\n");
		}
	    }
	}
    }
    if (!parent)
	return NULL;
    for (h = rl->heads; h; h = h->next)
	if (parent->name == h->name)
	    return h;
    fprintf (stderr, "Reference missing in merge: %s\n", parent->name);
    return NULL;
}

rev_list *
rev_list_merge (rev_list *head)
{
    int		count = rev_list_count (head);
    rev_list	*rl = calloc (1, sizeof (rev_list));
    rev_list	*l;
    rev_ref	*lh, *h;
    rev_ref	*lt, *t;
    rev_ref	**refs = calloc (count, sizeof (rev_commit *));
    int		nref;

    /*
     * Find all of the heads across all of the incoming trees
     * Yes, this is currently very inefficient
     */
    for (l = head; l; l = l->next) {
	for (lh = l->heads; lh; lh = lh->next) {
	    h = rev_find_head (rl, lh->name);
	    if (!h)
		rev_list_add_head (rl, NULL, lh->name, lh->degree);
	    else if (lh->degree > h->degree)
		h->degree = lh->degree;
	}
    }
    /*
     * Sort by degree so that finding branch points always works
     */
    rl->heads = rev_ref_sel_sort (rl->heads);
//    for (h = rl->heads; h; h = h->next)
//	fprintf (stderr, "head %s(%d)\n", h->name, h->degree);
    /*
     * Find branch parent relationships
     */
    for (h = rl->heads; h; h = h->next) {
	rev_ref_set_parent (rl, h, head);
	dump_ref_name (stderr, h);
	fprintf (stderr, "\n");
    }
    /*
     * Merge common branches
     */
    for (h = rl->heads; h; h = h->next) {
	/*
	 * Locate branch in every tree
	 */
	nref = 0;
	for (l = head; l; l = l->next) {
	    lh = rev_find_head (l, h->name);
	    if (lh)
		refs[nref++] = lh;
	}
	if (nref)
	    rev_branch_merge (refs, nref, h, rl);
    }
    /*
     * Compute 'tail' values
     */
    rev_list_set_tail (rl);
    /*
     * Compute set of tags
     */
    for (l = head; l; l = l->next) {
	for (lt = l->tags; lt; lt = lt->next) {
	    t = rev_find_tag (rl, lt->name);
	    if (!t)
		rev_list_add_tag (rl, NULL, lt->name, lt->degree);
	    else if (lt->degree == t->degree)
		t->degree = lt->degree;
	}
    }
    rl->tags = rev_ref_sel_sort (rl->tags);
    /*
     * Find tag locations
     */
    for (t = rl->tags; t; t = t->next) {
	/*
	 * Locate branch in every tree
	 */
	nref = 0;
	for (l = head; l; l = l->next) {
	    lh = rev_find_tag (l, t->name);
	    if (lh)
		refs[nref++] = lh;
	}
	if (nref) {
	    rev_tag_search (refs, nref, t, rl);
	}
	if (!t->commit)
	    fprintf (stderr, "lost tag %s\n", t->name);
	else
	    t->commit->tagged = 1;
    }
    rev_list_validate (rl);
    return rl;
}

/*
 * Icky. each file revision may be referenced many times in a single
 * tree. When freeing the tree, queue the file objects to be deleted
 * and clean them up afterwards
 */

static rev_file *rev_files;

static void
rev_file_mark_for_free (rev_file *f)
{
    if (f->name) {
	f->name = NULL;
	f->link = rev_files;
	rev_files = f;
    }
}

static void
rev_file_free_marked (void)
{
    rev_file	*f, *n;

    for (f = rev_files; f; f = n)
    {
	n = f->link;
	free (f);
    }
    rev_files = NULL;
}

rev_file *
rev_file_rev (char *name, cvs_number *n, time_t date)
{
    rev_file	*f = calloc (1, sizeof (rev_file));

    f->name = name;
    f->number = *n;
    f->date = date;
    return f;
}

void
rev_file_free (rev_file *f)
{
    free (f);
}

static void
rev_commit_free (rev_commit *commit, int free_files)
{
    rev_commit	*c;

    while ((c = commit)) {
	commit = c->parent;
	if (--c->seen == 0)
	{
	    if (free_files) {
		int i;
		for (i = 0; i < c->nfiles; i++)
		    rev_file_mark_for_free (c->files[i]);
	    }
	    free (c);
	}
    }
}

static void
rev_ref_free (rev_ref *ref)
{
    rev_ref	*r;

    while ((r = ref)) {
	ref = r->next;
	free (r);
    }
}

void
rev_head_free (rev_ref *head, int free_files)
{
    rev_ref	*h;

    while ((h = head)) {
	head = h->next;
	rev_commit_free (h->commit, free_files);
	free (h);
    }
}

void
rev_list_free (rev_list *rl, int free_files)
{
    rev_head_free (rl->heads, free_files);
    rev_ref_free (rl->tags);
    if (free_files)
	rev_file_free_marked ();
    free (rl);
}

void
rev_list_validate (rev_list *rl)
{
    rev_ref	*h;
    rev_commit	*c;
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	for (c = h->commit; c && c->parent; c = c->parent) {
	    if (c->tail)
		break;
//	    assert (time_compare (c->date, c->parent->date) >= 0);
	}
    }
}
