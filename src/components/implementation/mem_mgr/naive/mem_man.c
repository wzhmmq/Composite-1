/**
 * Copyright 2008 by Gabriel Parmer, gabep1@cs.bu.edu.  All rights
 * reserved.
 *
 * The George Washington University, Gabriel Parmer, gparmer@gwu.edu.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

/* 
 * FIXME: locking!
 */

#define COS_FMT_PRINT
#include <cos_component.h>
#include <cos_debug.h>
#include <print.h>

#include <mem_mgr.h>

#define MAX_ALIASES 32

#define MEM_MARKED 1

struct mapping_info {
	unsigned short int owner_spd, flags;
	vaddr_t addr;
	int parent;
};
struct mem_cell {
	int naliases;
	struct mapping_info map[MAX_ALIASES];
} __attribute__((packed));

static struct mem_cell cells[COS_MAX_MEMORY];

static inline long cell_index(struct mem_cell *c)
{
	return c - cells;
}

static inline struct mem_cell *find_unused(void)
{
	int i;

	/* If we care about scaling, this should, of course use freelist */
	for (i = 0 ; i < COS_MAX_MEMORY ; i++) {
		if (!cells[i].naliases) return &cells[i];
	}
	return NULL;
}

#define CACHE_SIZE 256
static struct mapping_cache {
	spdid_t spdid;
	vaddr_t addr;
	struct mem_cell *cell;
	int alias_num;
} cache[CACHE_SIZE];
int cache_head = 0;

static inline struct mapping_cache *cache_lookup(spdid_t spdid, vaddr_t addr)
{
	int i;

	for (i = 0 ; i < CACHE_SIZE ; i++) {
		struct mapping_cache *c = &cache[i];
		if (c->spdid == spdid && c->addr == addr) {
			assert(c->cell);
			return c;
		}
	}
	return NULL;
}

static inline void cache_add(spdid_t spdid, vaddr_t addr, struct mem_cell *mc, int alias)
{
	struct mapping_cache *c = &cache[cache_head];
	assert(cache_head < CACHE_SIZE);
	assert(mc);
	assert(spdid > 0);

	c->spdid = spdid;
	c->addr = addr;
	c->cell = mc;
	c->alias_num = alias;
	cache_head = (cache_head + 1) == CACHE_SIZE ? 0 : cache_head + 1;
}

static inline void cache_remove(struct mapping_cache *entry)
{
	assert(entry);

	entry->spdid = 0;
	cache_head = entry-cache;
}

static inline struct mem_cell *find_cell(spdid_t spd, vaddr_t addr, int *alias, int use_cache)
{
	int i, j;
	static int last_found = 0;
	int start_looking;
	struct mapping_cache *entry;

	if (likely(use_cache)) {
		entry = cache_lookup(spd, addr);
		if (entry) {
			*alias = entry->alias_num;
			return entry->cell;
		}
	}
	
	start_looking = last_found - 150;
	if (start_looking < 0) start_looking = 0;

	for (i = start_looking ; i < COS_MAX_MEMORY ; i++) {
		struct mem_cell *c = &cells[i];

		for (j = 0; j < MAX_ALIASES; j++) {
			if (c->map[j].owner_spd == spd && 
			    c->map[j].addr      == addr) {
				*alias = j;
				last_found = i;
				if (entry) {
					assert(entry->alias_num == j);
					assert(entry->cell == c);
				}
				return c;
			}
		}
	}
	for (i = 0 ; i < start_looking ; i++) {
		struct mem_cell *c = &cells[i];

		for (j = 0; j < MAX_ALIASES; j++) {
			if (c->map[j].owner_spd == spd && 
			    c->map[j].addr      == addr) {
				*alias = j;
				last_found = i;
				if (entry) {
					assert(entry->alias_num == j);
					assert(entry->cell == c);
				}
				return c;
			}
		}
	}

	return NULL;
}

/* 
 * Call to get a page of memory at a location.
 */
vaddr_t mman_get_page(spdid_t spd, vaddr_t addr, int flags)
{
	struct mem_cell *c;
	struct mapping_info *m;

	c = find_unused();
	if (!c) {
		printc("mm: no more available pages!\n");
		goto err;
	}

	c->naliases++;
	m = c->map;
	m->owner_spd = spd;
	m->addr = addr;
	m->parent = -1;

	/* Here we check for overwriting an already established mapping. */
	if (cos_mmap_cntl(COS_MMAP_GRANT, 0, spd, addr, cell_index(c))) {
		printc("mm: could not grant page @ %x to spd %d\n", 
		       (unsigned int)addr, (unsigned int)spd);
		m->owner_spd = m->addr = 0;
		goto err;
	}

	cache_add(spd, addr, c, 0);

	return addr;
err:
	return 0;
}

/* 
 * Make an alias to a page in a source spd @ a source address to a
 * destination spd/addr
 */
vaddr_t mman_alias_page(spdid_t s_spd, vaddr_t s_addr, spdid_t d_spd, vaddr_t d_addr)
{
	int alias = -1, i;
	struct mem_cell *c;
	struct mapping_info *base;

	c = find_cell(s_spd, s_addr, &alias, 1);

	if (-1 == alias) {printc("WTF \n");goto err;}
	assert(alias >= 0 && alias < MAX_ALIASES);
	base = c->map;
	for (i = 0 ; i < MAX_ALIASES ; i++) {
		if (alias == i || base[i].owner_spd != 0 || base[i].addr != 0) {
			continue;
		}
		
		if (cos_mmap_cntl(COS_MMAP_GRANT, 0, d_spd, d_addr, cell_index(c))) {
			printc("mm: could not alias page @ %x to spd %d from %x(%d)\n", 
			       (unsigned int)d_addr, (unsigned int)d_spd, (unsigned int)s_addr, (unsigned int)s_spd);
			goto err;
		}
		base[i].owner_spd = d_spd;
		base[i].addr = d_addr;
		base[i].parent = alias;
		c->naliases++;
		cache_add(d_spd, d_addr, c, i);

		return d_addr;
	}
	/* no available alias slots! */
err:
	return 0;
}

static inline int
is_descendent(struct mapping_info *mi, int parent, int child)
{
	assert(child < MAX_ALIASES && child >= 0);	
	while (mi[child].parent != -1) {
		assert(mi[child].parent < MAX_ALIASES && mi[child].parent >= 0);
		if (mi[child].parent == parent) return 1;
		child = mi[child].parent;
	}
	return 0;
}

/*
 * Call to give up a page of memory in an spd at an address.
 */
void mman_revoke_page(spdid_t spd, vaddr_t addr, int flags)
{
	int alias, i;
	struct mem_cell *mc;
	struct mapping_info *mi;

	mc = find_cell(spd, addr, &alias, 1);
	
	if (!mc) {
		/* FIXME: add return codes to this call */
		return;
	}
	mi = mc->map;
	for (i = 0 ; i < MAX_ALIASES ; i++) {
		int idx;
		struct mapping_cache *cache;

		if (i == alias || !mi[i].owner_spd || 
		    !is_descendent(mi, alias, i)) continue;
		idx = cos_mmap_cntl(COS_MMAP_REVOKE, 0, mi[i].owner_spd, 
				    mi[i].addr, 0);
		assert(&cells[idx] == mc);
		if ((cache = cache_lookup(mi[i].owner_spd, mi[i].addr))) cache_remove(cache);

		/* mark page as removed */
		mi[i].addr = 0;
		mc->naliases--;
	}
	/* Go through and free all pages marked as removed */
	for (i = 0 ; i < MAX_ALIASES ; i++) {
		if (mi[i].addr == 0 && 
		    mi[i].owner_spd) {
			mi[i].owner_spd = 0;
			mi[i].parent = 0;
		}
	}

	return;
}

/* 
 * FIXME: change interface to include the component making the call to
 * make sure that it owns the page it is trying to unmap (and the one
 * it is unmapping is a descendent.
 */
void mman_release_page(spdid_t spd, vaddr_t addr, int flags)
{
	int alias = -1;
	long idx;
	struct mem_cell *mc;
	struct mapping_info *mi;
	struct mapping_cache *cache_entry;

	mman_revoke_page(spd, addr, flags);

	cache_entry = cache_lookup(spd, addr);
	if (cache_entry) {
		alias = cache_entry->alias_num;
		mc = cache_entry->cell;
		cache_remove(cache_entry);
	} else {
		mc = find_cell(spd, addr, &alias, 0);
	}
	if (!mc) {
		/* FIXME: add return codes to this call */
		return;
	}
	mi = mc->map;
	idx = cos_mmap_cntl(COS_MMAP_REVOKE, 0, mi[alias].owner_spd, 
			    mi[alias].addr, 0);
	assert(&cells[idx] == mc);
	mi[alias].addr = 0;
	mi[alias].owner_spd = 0;
	mi[alias].parent = 0;
	mc->naliases--;
	if (cache_entry) cache_remove(cache_entry);

	return;
}

void mman_print_stats(void)
{
	int i, j, k, l;

	printc("Memory allocation stats:\n");
	for (k = 0 ; k < COS_MAX_MEMORY ; k++) {
		for (l = 0 ; l < MAX_ALIASES ; l++) {
			int spd_accum = 0, curr_spd;
			struct mapping_info *mc;

			mc = &cells[k].map[l];
			
			if (MEM_MARKED & mc->flags) continue;
			mc->flags |= MEM_MARKED;
			curr_spd = mc->owner_spd;
			spd_accum++;
			for (i = k ; i < COS_MAX_MEMORY ; i++) {
				for (j = 0 ; j < MAX_ALIASES ; j++) {
					mc = &cells[i].map[j];
					if (mc->owner_spd == curr_spd && !(MEM_MARKED & mc->flags)) {
						mc->flags |= MEM_MARKED;
						spd_accum++;
					}
				}
			}
			
			printc("\tspd %d used %d pages\n", 
			       (unsigned int)curr_spd, (unsigned int)spd_accum);
		}
	}
}

/* 
 * FIXME: add calls to obtain descriptors for the page regions, so
 * that they can be used to produce aliases.  This will allow for
 * shared memory, which we don't really support quite yet.
 */
