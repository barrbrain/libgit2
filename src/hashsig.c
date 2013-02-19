/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "hashsig.h"
#include "fileops.h"

typedef uint32_t hashsig_t;
typedef uint64_t hashsig_state;

#define HASHSIG_SCALE 100

#define HASHSIG_HASH_WINDOW 8
#define HASHSIG_HASH_START	0
#define HASHSIG_HASH_SHIFT  3
#define HASHSIG_HASH_MASK   0x000FFFFF

#define HASHSIG_HEAP_SIZE ((1 << 7) - 1)

typedef int (*hashsig_cmp)(const void *a, const void *b);

typedef struct {
	int size, asize;
	hashsig_cmp cmp;
	hashsig_t values[HASHSIG_HEAP_SIZE];
} hashsig_heap;

typedef struct {
	hashsig_state state, shift_n;
	char window[HASHSIG_HASH_WINDOW];
	int win_len, win_pos, saw_lf;
} hashsig_in_progress;

#define HASHSIG_IN_PROGRESS_INIT { HASHSIG_HASH_START, 1, {0}, 0, 0, 1 }

struct git_hashsig {
	hashsig_heap mins;
	hashsig_heap maxs;
	git_hashsig_option_t opt;
	int considered;
};

#define HEAP_LCHILD_OF(I) (((I)*2)+1)
#define HEAP_RCHILD_OF(I) (((I)*2)+2)
#define HEAP_PARENT_OF(I) (((I)-1)>>1)

static void hashsig_heap_init(hashsig_heap *h, hashsig_cmp cmp)
{
	h->size  = 0;
	h->asize = HASHSIG_HEAP_SIZE;
	h->cmp   = cmp;
}

static int hashsig_cmp_max(const void *a, const void *b)
{
	hashsig_t av = *(const hashsig_t *)a, bv = *(const hashsig_t *)b;
	return (av < bv) ? -1 : (av > bv) ? 1 : 0;
}

static int hashsig_cmp_min(const void *a, const void *b)
{
	hashsig_t av = *(const hashsig_t *)a, bv = *(const hashsig_t *)b;
	return (av > bv) ? -1 : (av < bv) ? 1 : 0;
}

static void hashsig_heap_up(hashsig_heap *h, int el)
{
	int parent_el = HEAP_PARENT_OF(el);

	while (el > 0 && h->cmp(&h->values[parent_el], &h->values[el]) > 0) {
		hashsig_t t = h->values[el];
		h->values[el] = h->values[parent_el];
		h->values[parent_el] = t;

		el = parent_el;
		parent_el = HEAP_PARENT_OF(el);
	}
}

static void hashsig_heap_down(hashsig_heap *h, int el)
{
	hashsig_t v, lv, rv;

	/* 'el < h->size / 2' tests if el is bottom row of heap */

	while (el < h->size / 2) {
		int lel = HEAP_LCHILD_OF(el), rel = HEAP_RCHILD_OF(el), swapel;

		v  = h->values[el];
		lv = h->values[lel];
		rv = h->values[rel];

		if (h->cmp(&v, &lv) < 0 && h->cmp(&v, &rv) < 0)
			break;

		swapel = (h->cmp(&lv, &rv) < 0) ? lel : rel;

		h->values[el] = h->values[swapel];
		h->values[swapel] = v;

		el = swapel;
	}
}

static void hashsig_heap_sort(hashsig_heap *h)
{
	/* only need to do this at the end for signature comparison */
	qsort(h->values, h->size, sizeof(hashsig_t), h->cmp);
}

static void hashsig_heap_insert(hashsig_heap *h, hashsig_t val)
{
	/* if heap is full, pop top if new element should replace it */
	if (h->size == h->asize && h->cmp(&val, &h->values[0]) > 0) {
		h->size--;
		h->values[0] = h->values[h->size];
		hashsig_heap_down(h, 0);
	}

	/* if heap is not full, insert new element */
	if (h->size < h->asize) {
		h->values[h->size++] = val;
		hashsig_heap_up(h, h->size - 1);
	}
}

GIT_INLINE(bool) hashsig_include_char(
	char ch, git_hashsig_option_t opt, int *saw_lf)
{
	if ((opt & GIT_HASHSIG_IGNORE_WHITESPACE) && git__isspace(ch))
		return false;

	if (opt & GIT_HASHSIG_SMART_WHITESPACE) {
		if (ch == '\r' || (*saw_lf && git__isspace(ch)))
			return false;

		*saw_lf = (ch == '\n');
	}

	return true;
}

/* This function is in the public domain.
 * Murmur3 was created by Austin Appleby,
 * and the C port and general tidying up
 * was done by Peter Scott. */
static inline uint32_t fmix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;

	return h;
}

static void hashsig_initial_window(
	git_hashsig *sig,
	const char **data,
	size_t size,
	hashsig_in_progress *prog)
{
	hashsig_state state, shift_n;
	int win_len;
	const char *scan, *end;

	/* init until we have processed at least HASHSIG_HASH_WINDOW data */

	if (prog->win_len >= HASHSIG_HASH_WINDOW)
		return;

	state   = prog->state;
	win_len = prog->win_len;
	shift_n = prog->shift_n;

	scan = *data;
	end  = scan + size;

	while (scan < end && win_len < HASHSIG_HASH_WINDOW) {
		unsigned char ch = *scan++;

		if (!hashsig_include_char(ch, sig->opt, &prog->saw_lf))
			continue;

		state = (state & ~HASHSIG_HASH_MASK) | ((state * HASHSIG_HASH_SHIFT + ch) & HASHSIG_HASH_MASK);
		state += ch << 20;

		if (!win_len)
			shift_n = 1;
		else
			shift_n = (shift_n * HASHSIG_HASH_SHIFT) & HASHSIG_HASH_MASK;

		prog->window[win_len++] = ch;
	}

	/* insert initial hash if we just finished */

	if (win_len == HASHSIG_HASH_WINDOW) {
		hashsig_heap_insert(&sig->mins, fmix32(state));
		hashsig_heap_insert(&sig->maxs, fmix32(state));
		sig->considered = 1;
	}

	prog->state   = state;
	prog->win_len = win_len;
	prog->shift_n = shift_n;

	*data = scan;
}

static int hashsig_add_hashes(
	git_hashsig *sig,
	const char *data,
	size_t size,
	hashsig_in_progress *prog)
{
	const char *scan = data, *end = data + size;
	hashsig_state state, shift_n, rmv;

	if (prog->win_len < HASHSIG_HASH_WINDOW)
		hashsig_initial_window(sig, &scan, size, prog);

	state   = prog->state;
	shift_n = prog->shift_n;

	/* advance window, adding new chars and removing old */

	for (; scan < end; ++scan) {
		unsigned char ch = *scan;

		if (!hashsig_include_char(ch, sig->opt, &prog->saw_lf))
			continue;

		rmv = (shift_n + (1 << 20)) * prog->window[prog->win_pos];

		state = (state - rmv);
		state = (state & ~HASHSIG_HASH_MASK) | ((state * HASHSIG_HASH_SHIFT) & HASHSIG_HASH_MASK);
		state = state + ch + (ch << 20);

		hashsig_heap_insert(&sig->mins, fmix32(state));
		hashsig_heap_insert(&sig->maxs, fmix32(state));
		sig->considered++;

		prog->window[prog->win_pos] = ch;
		prog->win_pos = (prog->win_pos + 1) % HASHSIG_HASH_WINDOW;
	}

	prog->state = state;

	return 0;
}

static int hashsig_finalize_hashes(git_hashsig *sig)
{
	if (sig->mins.size < HASHSIG_HEAP_SIZE) {
		giterr_set(GITERR_INVALID,
			"File too small for similarity signature calculation");
		return GIT_EBUFS;
	}

	hashsig_heap_sort(&sig->mins);
	hashsig_heap_sort(&sig->maxs);

	return 0;
}

static git_hashsig *hashsig_alloc(git_hashsig_option_t opts)
{
	git_hashsig *sig = git__calloc(1, sizeof(git_hashsig));
	if (!sig)
		return NULL;

	hashsig_heap_init(&sig->mins, hashsig_cmp_min);
	hashsig_heap_init(&sig->maxs, hashsig_cmp_max);
	sig->opt = opts;

	return sig;
}

int git_hashsig_create(
	git_hashsig **out,
	const git_buf *buf,
	git_hashsig_option_t opts)
{
	int error;
	hashsig_in_progress prog = HASHSIG_IN_PROGRESS_INIT;
	git_hashsig *sig = hashsig_alloc(opts);
	GITERR_CHECK_ALLOC(sig);

	error = hashsig_add_hashes(sig, buf->ptr, buf->size, &prog);

	if (!error)
		error = hashsig_finalize_hashes(sig);

	if (!error)
		*out = sig;
	else
		git_hashsig_free(sig);

	return error;
}

int git_hashsig_create_fromfile(
	git_hashsig **out,
	const char *path,
	git_hashsig_option_t opts)
{
	char buf[4096];
	ssize_t buflen = 0;
	int error = 0, fd;
	hashsig_in_progress prog = HASHSIG_IN_PROGRESS_INIT;
	git_hashsig *sig = hashsig_alloc(opts);
	GITERR_CHECK_ALLOC(sig);

	if ((fd = git_futils_open_ro(path)) < 0) {
		git__free(sig);
		return fd;
	}

	while (!error) {
		if ((buflen = p_read(fd, buf, sizeof(buf))) <= 0) {
			if ((error = buflen) < 0)
				giterr_set(GITERR_OS,
					"Read error on '%s' calculating similarity hashes", path);
			break;
		}

		error = hashsig_add_hashes(sig, buf, buflen, &prog);
	}

	p_close(fd);

	if (!error)
		error = hashsig_finalize_hashes(sig);

	if (!error)
		*out = sig;
	else
		git_hashsig_free(sig);

	return error;
}

void git_hashsig_free(git_hashsig *sig)
{
	git__free(sig);
}

static int hashsig_heap_compare(const hashsig_heap *a, const hashsig_heap *b)
{
	int matches = 0, i, j, cmp;

	assert(a->cmp == b->cmp);

	/* hash heaps are sorted - just look for overlap vs total */

	for (i = 0, j = 0; i < a->size && j < b->size; ) {
		cmp = a->cmp(&a->values[i], &b->values[j]);

		if (cmp < 0)
			++i;
		else if (cmp > 0)
			++j;
		else {
			++i; ++j; ++matches;
		}
	}

	return HASHSIG_SCALE * (matches * 2) / (a->size + b->size);
}

int git_hashsig_compare(const git_hashsig *a, const git_hashsig *b)
{
	return (hashsig_heap_compare(&a->mins, &b->mins) +
			hashsig_heap_compare(&a->maxs, &b->maxs)) / 2;
}
