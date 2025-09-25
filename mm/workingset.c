// SPDX-License-Identifier: GPL-2.0
/*
 * Workingset detection
 *
 * Copyright (C) 2013 Red Hat, Inc., Johannes Weiner
 */

#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/writeback.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/xarray.h>

/*DJL ADD BEGIN*/
#include <trace/events/lru_gen.h>
/*DJL ADD END*/

/* MULTISWAP FIX: Counters for shadow entry accounting */
atomic_long_t shadow_entries_created = ATOMIC_LONG_INIT(0);
atomic_long_t shadow_entries_freed = ATOMIC_LONG_INIT(0);
atomic_long_t shadow_entries_cleaned = ATOMIC_LONG_INIT(0);

/* MULTISWAP FIX: Functions to read shadow entry counters */
long get_shadow_entries_created(void)
{
	return atomic_long_read(&shadow_entries_created);
}

long get_shadow_entries_freed(void)
{
	return atomic_long_read(&shadow_entries_freed);
}

long get_shadow_entries_cleaned(void)
{
	return atomic_long_read(&shadow_entries_cleaned);
}

long get_shadow_entries_balance(void)
{
	return atomic_long_read(&shadow_entries_created) - 
	       atomic_long_read(&shadow_entries_freed) - 
	       atomic_long_read(&shadow_entries_cleaned);
}


/*
 *		Double CLOCK lists
 *
 * Per node, two clock lists are maintained for file pages: the
 * inactive and the active list.  Freshly faulted pages start out at
 * the head of the inactive list and page reclaim scans pages from the
 * tail.  Pages that are accessed multiple times on the inactive list
 * are promoted to the active list, to protect them from reclaim,
 * whereas active pages are demoted to the inactive list when the
 * active list grows too big.
 *
 *   fault ------------------------+
 *                                 |
 *              +--------------+   |            +-------------+
 *   reclaim <- |   inactive   | <-+-- demotion |    active   | <--+
 *              +--------------+                +-------------+    |
 *                     |                                           |
 *                     +-------------- promotion ------------------+
 *
 *
 *		Access frequency and refault distance
 *
 * A workload is thrashing when its pages are frequently used but they
 * are evicted from the inactive list every time before another access
 * would have promoted them to the active list.
 *
 * In cases where the average access distance between thrashing pages
 * is bigger than the size of memory there is nothing that can be
 * done - the thrashing set could never fit into memory under any
 * circumstance.
 *
 * However, the average access distance could be bigger than the
 * inactive list, yet smaller than the size of memory.  In this case,
 * the set could fit into memory if it weren't for the currently
 * active pages - which may be used more, hopefully less frequently:
 *
 *      +-memory available to cache-+
 *      |                           |
 *      +-inactive------+-active----+
 *  a b | c d e f g h i | J K L M N |
 *      +---------------+-----------+
 *
 * It is prohibitively expensive to accurately track access frequency
 * of pages.  But a reasonable approximation can be made to measure
 * thrashing on the inactive list, after which refaulting pages can be
 * activated optimistically to compete with the existing active pages.
 *
 * Approximating inactive page access frequency - Observations:
 *
 * 1. When a page is accessed for the first time, it is added to the
 *    head of the inactive list, slides every existing inactive page
 *    towards the tail by one slot, and pushes the current tail page
 *    out of memory.
 *
 * 2. When a page is accessed for the second time, it is promoted to
 *    the active list, shrinking the inactive list by one slot.  This
 *    also slides all inactive pages that were faulted into the cache
 *    more recently than the activated page towards the tail of the
 *    inactive list.
 *
 * Thus:
 *
 * 1. The sum of evictions and activations between any two points in
 *    time indicate the minimum number of inactive pages accessed in
 *    between.
 *
 * 2. Moving one inactive page N page slots towards the tail of the
 *    list requires at least N inactive page accesses.
 *
 * Combining these:
 *
 * 1. When a page is finally evicted from memory, the number of
 *    inactive pages accessed while the page was in cache is at least
 *    the number of page slots on the inactive list.
 *
 * 2. In addition, measuring the sum of evictions and activations (E)
 *    at the time of a page's eviction, and comparing it to another
 *    reading (R) at the time the page faults back into memory tells
 *    the minimum number of accesses while the page was not cached.
 *    This is called the refault distance.
 *
 * Because the first access of the page was the fault and the second
 * access the refault, we combine the in-cache distance with the
 * out-of-cache distance to get the complete minimum access distance
 * of this page:
 *
 *      NR_inactive + (R - E)
 *
 * And knowing the minimum access distance of a page, we can easily
 * tell if the page would be able to stay in cache assuming all page
 * slots in the cache were available:
 *
 *   NR_inactive + (R - E) <= NR_inactive + NR_active
 *
 * which can be further simplified to
 *
 *   (R - E) <= NR_active
 *
 * Put into words, the refault distance (out-of-cache) can be seen as
 * a deficit in inactive list space (in-cache).  If the inactive list
 * had (R - E) more page slots, the page would not have been evicted
 * in between accesses, but activated instead.  And on a full system,
 * the only thing eating into inactive list space is active pages.
 *
 *
 *		Refaulting inactive pages
 *
 * All that is known about the active list is that the pages have been
 * accessed more than once in the past.  This means that at any given
 * time there is actually a good chance that pages on the active list
 * are no longer in active use.
 *
 * So when a refault distance of (R - E) is observed and there are at
 * least (R - E) active pages, the refaulting page is activated
 * optimistically in the hope that (R - E) active pages are actually
 * used less frequently than the refaulting page - or even not used at
 * all anymore.
 *
 * That means if inactive cache is refaulting with a suitable refault
 * distance, we assume the cache workingset is transitioning and put
 * pressure on the current active list.
 *
 * If this is wrong and demotion kicks in, the pages which are truly
 * used more frequently will be reactivated while the less frequently
 * used once will be evicted from memory.
 *
 * But if this is right, the stale pages will be pushed out of memory
 * and the used pages get to stay in cache.
 *
 *		Refaulting active pages
 *
 * If on the other hand the refaulting pages have recently been
 * deactivated, it means that the active list is no longer protecting
 * actively used cache from reclaim. The cache is NOT transitioning to
 * a different workingset; the existing workingset is thrashing in the
 * space allocated to the page cache.
 *
 *
 *		Implementation
 *
 * For each node's LRU lists, a counter for inactive evictions and
 * activations is maintained (node->nonresident_age).
 *
 * On eviction, a snapshot of this counter (along with some bits to
 * identify the node) is stored in the now empty page cache
 * slot of the evicted page.  This is called a shadow entry.
 *
 * On cache misses for which there are shadow entries, an eligible
 * refault distance will immediately activate the refaulting page.
 */

#define WORKINGSET_SHIFT 1
#define EVICTION_SHIFT	((BITS_PER_LONG - BITS_PER_XA_VALUE) +	\
			 WORKINGSET_SHIFT + NODES_SHIFT + \
			 MEM_CGROUP_ID_SHIFT)
#define EVICTION_MASK	(~0UL >> EVICTION_SHIFT)

/*
 * Eviction timestamps need to be able to cover the full range of
 * actionable refaults. However, bits are tight in the xarray
 * entry, and after storing the identifier for the lruvec there might
 * not be enough left to represent every single actionable refault. In
 * that case, we have to sacrifice granularity for distance, and group
 * evictions into coarser buckets by shaving off lower timestamp bits.
 */
static unsigned int bucket_order __read_mostly;

static struct kmem_cache *shadow_entry_cache;
struct kmem_cache * get_shadow_entry_cache(void){return shadow_entry_cache;}

const unsigned int shadow_entry_magic = 0x4321;
const unsigned int shadow_entry_invalidmagic = 0x8765;

/* Helper function to count shadow entry timeout events */
void count_shadow_timeout_event(struct shadow_entry *entry_ext)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long shadow_val;
	int memcg_id;

	if (!entry_ext || !entry_ext->shadow)
		return;

	/* Extract memcg from shadow entry */
	shadow_val = xa_to_value(entry_ext->shadow);
	shadow_val >>= WORKINGSET_SHIFT;
	shadow_val >>= NODES_SHIFT;
	memcg_id = shadow_val & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);

	/* Get memcg and increment counter for shadow timeout without refault */
	rcu_read_lock();
	memcg = mem_cgroup_from_id(memcg_id);
	if (memcg)
		memcg_memory_event(memcg, MEMCG_SHADOW_TIMEOUT_NO_REFAULT);
	rcu_read_unlock();
}

int entry_is_entry_ext(const void *entry){
	if (unlikely(!entry))
		return -2;
	if (unlikely(xa_is_value(entry)))
		return 0;
	unsigned short magic = READ_ONCE(((struct shadow_entry*)entry)->magic);
	if (magic != (unsigned short)((shadow_entry_magic) ^ ((unsigned long)entry & 0xFFFF))){
		// if (((struct shadow_entry*)entry)->magic == shadow_entry_invalidmagic)
		// 	pr_err("entry was invalied ext[%lx]",entry);
		if (magic == 0xFFFF){
			// pr_err("entry_is_entry_ext ext[%lx] has been freed", entry);
			return -1;
		}
		else if (magic != (unsigned short)((unsigned long)entry & 0xFFFF)){
			return 0;
		} else {
			// pr_err("entry was poisoned ext[%lx] magic[%lx]",entry, ((struct shadow_entry*)entry)->magic);
			return 0;
		}
	}
	if (unlikely(!xa_is_value(((struct shadow_entry*)entry)->shadow))){
		// pr_err("entry_is_entry_ext !xa_is_value ext[%lx]",(unsigned long)entry);
		return 0;
	}
	return 1;
}
int entry_is_entry_ext_debug(const void *entry){
	if (!entry){
		pr_err("entry_is_entry_ext_debug entry[%p] NULL", entry);
		return -2;	
	}
	if (xa_is_value(entry)) {
		pr_err("entry_is_entry_ext_debug entry[%p] xa_is_value", entry);
		return 0;
	}
	if (((struct shadow_entry*)entry)->magic != (unsigned short)((shadow_entry_magic) ^ ((unsigned long)entry & 0xFFFF))){
		
		if (((struct shadow_entry*)entry)->magic == 0xFFFF){
			pr_err("entry_is_entry_ext_debug ext[%p] has been freed", entry);
			return -1;
		}else if (((struct shadow_entry*)entry)->magic != (unsigned short)((unsigned long)entry & 0xFFFF)) { // other stuff
			pr_err("entry was magic invalid ext[%p] magic[%x]",entry, ((struct shadow_entry*)entry)->magic);
			return 0;
		}else {
			pr_err("entry was poisoned ext[%p] magic[%x]",entry, ((struct shadow_entry*)entry)->magic);
			return 0;
		}
	}
	if (!xa_is_value(((struct shadow_entry*)entry)->shadow)){
		pr_err("entry_is_entry_ext !xa_is_value ext[%lx]",(unsigned long)entry);
		return 0;
	}
	return 1;
}

int entry_ext_memcg_id(struct shadow_entry* entry_ext){
	int _memcgid, _nid;
	bool _workingset;
	unsigned long _entry;
	if (!entry_ext || !(entry_is_entry_ext(entry_ext) > 0)){
		return -5;
	}
	_entry = xa_to_value(entry_ext->shadow);
	_workingset = _entry & ((1UL << WORKINGSET_SHIFT) - 1);
	_entry >>= WORKINGSET_SHIFT;
	_nid = _entry & ((1UL << NODES_SHIFT) - 1);
	_entry >>= NODES_SHIFT;
	_memcgid = _entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);
	return _memcgid;
}

/*
THIS will also free old_entry_ext
 */
static void *pack_shadow_ext(int memcgid, pg_data_t *pgdat, unsigned long eviction,
			 bool workingset, struct shadow_entry* entry_ext, unsigned long min_seq, struct shadow_entry* old_entry_ext,
			 struct folio* folio, swp_entry_t entry)
{
	int i;
	unsigned long _entry;
	int _memcgid, _nid;
	bool _workingset;
	eviction &= EVICTION_MASK;
	eviction = (eviction << MEM_CGROUP_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | pgdat->node_id;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;
	if (entry_ext){
		/* MULTISWAP FIX: Count shadow entry creation */
		atomic_long_inc(&shadow_entries_created);
		
		entry_ext->shadow = xa_mk_value(eviction);
		entry_ext->magic = shadow_entry_magic ^ (unsigned short)((unsigned long)entry_ext & 0xFFFF); //valid
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
		if (old_entry_ext){
			_entry = xa_to_value(old_entry_ext->shadow);
			_workingset = _entry & ((1UL << WORKINGSET_SHIFT) - 1);
			_entry >>= WORKINGSET_SHIFT;
			_nid = _entry & ((1UL << NODES_SHIFT) - 1);
			_entry >>= NODES_SHIFT;
			_memcgid = _entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);

			if (memcgid == _memcgid){ //match
				/* Transfer historical data from previous shadow entry */
				entry_ext->hist_ts[SE_HIST_REFAULT_COUNT] = old_entry_ext->hist_ts[SE_HIST_REFAULT_COUNT];
				entry_ext->hist_ts[SE_HIST_AVG_DISTANCE] = old_entry_ext->hist_ts[SE_HIST_AVG_DISTANCE];
				entry_ext->hist_ts[SE_HIST_PAGE_ID] = old_entry_ext->hist_ts[SE_HIST_PAGE_ID]; /* Preserve page_id */
				entry_ext->hist_ts[SE_HIST_EVICTION_TIME] = min_seq % 0xFFFF; /* Update eviction time */
				entry_ext->hist_ts[SE_HIST_STILL_HOT] = old_entry_ext->hist_ts[SE_HIST_STILL_HOT]; /* Copy still_hot flag */
			}
			else{
				/* Different memcg - initialize with conservative defaults scaled by 1000x */
				entry_ext->hist_ts[SE_HIST_REFAULT_COUNT] = 0;
				entry_ext->hist_ts[SE_HIST_AVG_DISTANCE] = SE_HIST_INITIAL_AVG_DIST * SE_HIST_SCALE_FACTOR;
				entry_ext->hist_ts[SE_HIST_PAGE_ID] = old_entry_ext->hist_ts[SE_HIST_PAGE_ID]; /* Preserve page_id from old entry */
				entry_ext->hist_ts[SE_HIST_EVICTION_TIME] = min_seq % 0xFFFF; /* Update eviction time */
				entry_ext->hist_ts[SE_HIST_STILL_HOT] = 0; /* Reset still_hot flag for different memcg */
			}
			trace_shadow_ext_transfer(folio, memcgid, entry_ext, old_entry_ext, entry.val);
			if (swp_entry_test_ext(entry))
				pr_info("[FREE]workingset_refualt entry[%lx]->folio[%p] ext[%lx](free)->[%lx] ok", 
							entry.val, folio, (unsigned long)old_entry_ext, (unsigned long)entry_ext);
			/* MULTISWAP FIX: Count shadow entry freeing */
			atomic_long_inc(&shadow_entries_freed);
			shadow_entry_free(old_entry_ext);
			// trace_shadow_entry_free(entry_ext, 12);	
		}
		else{
			/* No previous shadow entry - initialize with conservative defaults scaled by 1000x */
			entry_ext->hist_ts[SE_HIST_REFAULT_COUNT] = 0;
			entry_ext->hist_ts[SE_HIST_AVG_DISTANCE] = SE_HIST_INITIAL_AVG_DIST * SE_HIST_SCALE_FACTOR;
			/* Keep the page_id assigned during allocation - don't overwrite it */
			entry_ext->hist_ts[SE_HIST_EVICTION_TIME] = min_seq % 0xFFFF; /* Set eviction time */
			entry_ext->hist_ts[SE_HIST_STILL_HOT] = 0; /* Initialize still_hot flag for first-time entry */
		}
#endif
	}
	else{
		pr_err("entry_ext[%lx]->shadow[%lx]", (unsigned long)entry_ext, eviction);
		return NULL;
	}
	if (((unsigned long)entry_ext & 3) != 0) {
		pr_err("xarray err entry_ext[%lx]->shadow[%lx]", (unsigned long)entry_ext, eviction);
		BUG();
	}
	return entry_ext;////xa_mk_value((unsigned long)entry_ext);
}

static void unpack_shadow_ext(void *shadow, int *memcgidp, pg_data_t **pgdat,
			  unsigned long *evictionp, bool *workingsetp, unsigned long* last_hist)
{
	struct shadow_entry* entry_ext = NULL;
	unsigned long entry;
	int memcgid, nid;
	bool workingset;

	if (!shadow){
		pr_err("unpack_shadow_ext nullptr");
		BUG();
	}
	if (xa_is_value(shadow)){
		entry = xa_to_value(shadow);
		*last_hist = ULONG_MAX;
	}
	else if (entry_is_entry_ext_debug(shadow) > 0){
		entry_ext = (struct shadow_entry*)shadow;
		entry = xa_to_value(entry_ext->shadow);
		*last_hist = entry_ext->hist_ts[SE_HIST_AVG_DISTANCE];
	}
	else{
		pr_err("unpack_shadow_ext xa err %p", shadow);
		BUG();
	}

	workingset = entry & ((1UL << WORKINGSET_SHIFT) - 1);
	entry >>= WORKINGSET_SHIFT;
	nid = entry & ((1UL << NODES_SHIFT) - 1);
	entry >>= NODES_SHIFT;
	memcgid = entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);
	entry >>= MEM_CGROUP_ID_SHIFT;

	*memcgidp = memcgid;
	*pgdat = NODE_DATA(nid);
	*evictionp = entry;
	*workingsetp = workingset;
}

static void *pack_shadow(int memcgid, pg_data_t *pgdat, unsigned long eviction,
			 bool workingset)
{
	eviction &= EVICTION_MASK;
	eviction = (eviction << MEM_CGROUP_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | pgdat->node_id;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;

	return xa_mk_value(eviction);
}

static void unpack_shadow(void *shadow, int *memcgidp, pg_data_t **pgdat,
			  unsigned long *evictionp, bool *workingsetp)
{
	unsigned long entry = xa_to_value(shadow);
	int memcgid, nid;
	bool workingset;

	workingset = entry & ((1UL << WORKINGSET_SHIFT) - 1);
	entry >>= WORKINGSET_SHIFT;
	nid = entry & ((1UL << NODES_SHIFT) - 1);
	entry >>= NODES_SHIFT;
	memcgid = entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);
	entry >>= MEM_CGROUP_ID_SHIFT;

	*memcgidp = memcgid;
	*pgdat = NODE_DATA(nid);
	*evictionp = entry;
	*workingsetp = workingset;
}

#ifdef CONFIG_LRU_GEN

static void *lru_gen_eviction(struct folio *folio, int swap_level, long swap_space_left, struct shadow_entry* se, swp_entry_t entry)
{
	int hist;
	unsigned long token;
	unsigned long min_seq;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	int type = folio_is_file_lru(folio);
	int delta = folio_nr_pages(folio);
	int refs = folio_lru_refs(folio);
	int tier = lru_tier_from_refs(refs);
	struct mem_cgroup *memcg = folio_memcg(folio);
	struct pglist_data *pgdat = folio_pgdat(folio);
	void* ret;
	int is_se;
	BUILD_BUG_ON(LRU_GEN_WIDTH + LRU_REFS_WIDTH > BITS_PER_LONG - EVICTION_SHIFT);

	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	lrugen = &lruvec->lrugen;
	min_seq = READ_ONCE(lrugen->min_seq[type]);
	token = (min_seq << LRU_REFS_WIDTH) | max(refs - 1, 0);

	hist = lru_hist_from_seq(min_seq);
	atomic_long_add(delta, &lrugen->evicted[hist][type][tier]);

	if (!se){
		is_se = 0;
		ret = pack_shadow(mem_cgroup_id(memcg), pgdat, token, refs);
		if (!xa_is_value(ret)){
			pr_err("bug in pack_shadow");
		}
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
		if (folio->shadow_ext && entry_is_entry_ext(folio->shadow_ext) == 1){
			pr_err("bug2 in pack_shadow");
			/* MULTISWAP FIX: Count shadow entry freeing */
			atomic_long_inc(&shadow_entries_freed);
			shadow_entry_free(folio->shadow_ext);
			BUG();
		}
		// folio->shadow_ext = NULL;
		ASSERT_FOLIO_NO_SE(folio, __FILE__, __LINE__);
#endif
	}
	else{
		is_se = 1;
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
		struct shadow_entry* shadow = folio_remove_shadow_entry(folio);
		if (shadow){
			if	(entry_is_entry_ext(shadow) > 0){
				ret = pack_shadow_ext(mem_cgroup_id(memcg), pgdat, token, refs, se, min_seq, shadow, folio, entry);
			}
			else if (entry_is_entry_ext(shadow) < 0){ //already freed
				ret = pack_shadow_ext(mem_cgroup_id(memcg), pgdat, token, refs, se, min_seq, NULL, folio, entry);
			}
			else{ // == 0 broken
				pr_err("folio[%p]->has broken shadow_ext[%lx] %s:%d", 
					folio, (unsigned long)shadow, __FILE__, __LINE__);
				ret = pack_shadow_ext(mem_cgroup_id(memcg), pgdat, token, refs, se, min_seq, NULL, folio, entry);
			} 
		}
		else{
			// pr_err("folio[%lx] has no shadow_ext[%lx]", folio, folio->shadow_ext);
			ret = pack_shadow_ext(mem_cgroup_id(memcg), pgdat, token, refs, se, min_seq, NULL, folio, entry);
		}
#else
		ret = pack_shadow_ext(mem_cgroup_id(memcg), pgdat, token, refs, se, min_seq, NULL, folio, entry);
#endif
		// if (!xa_is_value(ret)){
		// 	pr_err("bug in pack_shadow");
		// }
		if (ret){
			if  (entry_is_entry_ext_debug(ret) < 1){
				pr_err("bug in pack_shadow_ext ret[%p] se[%p]", ret, se);
				BUG();				
			}
		}
	}
	
	if (!se)
		trace_folio_ws_chg(folio, 0, pgdat, (unsigned short)mem_cgroup_id(memcg), token, refs, 0, swap_level, swap_space_left, (unsigned long)entry.val);
	else{
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
		if (is_se)
			trace_folio_ws_chg_se(folio, 0, (unsigned short)mem_cgroup_id(memcg), token, refs, 0, swap_level, swap_space_left, ret, (unsigned long)entry.val);
		else
			trace_folio_ws_chg(folio, 0, pgdat, (unsigned short)mem_cgroup_id(memcg), token, refs, 0, swap_level, swap_space_left, (unsigned long)entry.val);
#else
		trace_folio_ws_chg(folio, 0, pgdat, (unsigned short)mem_cgroup_id(memcg), token, refs, 0, swap_level, swap_space_left, (unsigned long)entry.val);
#endif
	}
	ASSERT_FOLIO_NO_SE(folio, __FILE__, __LINE__);
	if (se){
		if (ret != se || entry_is_entry_ext_debug(se) < 1){
			pr_err("ret[%p] se[%p]", ret, se);
			BUG();
		}
	}
	return ret;
	// return pack_shadow(mem_cgroup_id(memcg), pgdat, token, refs);
}

static void lru_gen_refault(struct folio *folio, void *shadow, int* try_free_entry, unsigned long va, 
			int swap_level, unsigned long entry, bool* abandon_shadow)
{
	int hist, tier, refs;
	int memcg_id;
	bool workingset;
	unsigned long token;
	unsigned long min_seq;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	struct mem_cgroup *memcg;
	struct pglist_data *pgdat;
	int type = folio_is_file_lru(folio);
	int delta = folio_nr_pages(folio);
	/*DJL ADD BEGIN*/
	int dist = -1, dist_ret = -1;
	unsigned long lasthist = ULONG_MAX;
	/*DJL ADD END*/

	// unpack_shadow(shadow, &memcg_id, &pgdat, &token, &workingset);
	unpack_shadow_ext(shadow, &memcg_id, &pgdat, &token, &workingset, &lasthist);
	if (pgdat != folio_pgdat(folio))
		return;

	rcu_read_lock();

	memcg = folio_memcg_rcu(folio);
	if (memcg_id != mem_cgroup_id(memcg))
		goto unlock;

	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	lrugen = &lruvec->lrugen;

	min_seq = READ_ONCE(lrugen->min_seq[type]);
	/*DJL ADD BEGIN*/
	if (entry_is_entry_ext(shadow) > 0){
		struct shadow_entry* entry_ext = (struct shadow_entry*)shadow;
		unsigned int current_refault_dist;
		unsigned int refault_count;
		unsigned int old_avg_distance;
		unsigned int new_avg_distance;
		
		/* Calculate refault distance using separate eviction time field */
		current_refault_dist = (min_seq % 0xFFFF - entry_ext->hist_ts[SE_HIST_EVICTION_TIME]) % 0xFFFF;
		
		/* Update refault statistics */
		refault_count = entry_ext->hist_ts[SE_HIST_REFAULT_COUNT];
		old_avg_distance = entry_ext->hist_ts[SE_HIST_AVG_DISTANCE];
		
		if (refault_count == 0) {
			/* First refault - replace initial conservative assumption, scaled by 1000x */
			new_avg_distance = (unsigned int)((unsigned long)current_refault_dist * SE_HIST_SCALE_FACTOR);
		} else {
			/* Calculate new running average scaled by 1000x using unsigned long to prevent overflow */
			unsigned long temp_avg = ((unsigned long)old_avg_distance * refault_count + (unsigned long)current_refault_dist * SE_HIST_SCALE_FACTOR) / (refault_count + 1);
			new_avg_distance = (unsigned int)temp_avg;
		}
		
		/* Update shadow entry with new statistics */
		entry_ext->hist_ts[SE_HIST_REFAULT_COUNT] = refault_count + 1;
		entry_ext->hist_ts[SE_HIST_AVG_DISTANCE] = new_avg_distance;
		/* Note: SE_HIST_PAGE_ID remains constant throughout page lifetime */
		
		/* Use scaled value directly for better precision in decisions */
		lasthist = new_avg_distance;
	}

	if (lasthist == ULONG_MAX){
		lasthist = (token >> LRU_REFS_WIDTH) % MAX_NR_GENS;
		dist = lasthist;//(min_seq + MAX_NR_GENS - lasthist ) % MAX_NR_GENS;		
		dist_ret = dist;
	}
	else{
		/* Use the average distance for decision making */
		dist = lasthist;
		dist_ret = dist + MAX_NR_GENS;
	}
	swp_entry_t temp_entry;
	temp_entry.val = entry;
	struct swap_info_struct* info = swp_swap_info(temp_entry);
	if(!memcg || entry==(unsigned long)-1)
		goto skip_count;
	if (!info){
		pr_err("entry[%lx] err folio[%p]", temp_entry.val, folio);
		goto skip_count;
	}
	switch(dist){
		case 0: count_memcg_events(memcg, FRE0, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST0, 1); break;
		case 1: count_memcg_events(memcg, FRE1, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST1, 1); break;
		case 2: count_memcg_events(memcg, FRE2, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST2, 1); break;
		case 3: count_memcg_events(memcg, FRE3, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST3, 1); break;
		case 4: count_memcg_events(memcg, FRE4, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST4, 1); break;
		case 5: count_memcg_events(memcg, FRE5, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST5, 1); break;
		case 6: count_memcg_events(memcg, FRE6, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST6, 1); break;
		case 7: count_memcg_events(memcg, FRE7, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST7, 1); break;
		case 8: count_memcg_events(memcg, FRE8, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST8, 1); break;
		case 9: count_memcg_events(memcg, FRE9, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST9, 1); break;
		case 10: count_memcg_events(memcg, FRE10, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST10, 1); break;
		case 11: count_memcg_events(memcg, FRE11, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST11, 1); break;
		case 12: count_memcg_events(memcg, FRE12, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST12, 1); break;
		case 13: count_memcg_events(memcg, FRE13, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST13, 1); break;
		case 14: count_memcg_events(memcg, FRE14, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST14, 1); break;
		case 15: count_memcg_events(memcg, FRE15, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST15, 1); break;
		case 16: count_memcg_events(memcg, FRE16, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST16, 1); break;
		case 17: count_memcg_events(memcg, FRE17, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST17, 1); break;
		case 18: count_memcg_events(memcg, FRE18, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST18, 1); break;
		case 19: count_memcg_events(memcg, FRE19, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST19, 1); break;
		case 20: count_memcg_events(memcg, FRE20, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST20, 1); break;
		case 21: count_memcg_events(memcg, FRE21, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST21, 1); break;
		case 22: count_memcg_events(memcg, FRE22, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST22, 1); break;
		case 23: count_memcg_events(memcg, FRE23, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST23, 1); break;
		case 24: count_memcg_events(memcg, FRE24, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST24, 1); break;
		case 25: count_memcg_events(memcg, FRE25, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST25, 1); break;
		case 26: count_memcg_events(memcg, FRE26, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST26, 1); break;
		case 27: count_memcg_events(memcg, FRE27, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST27, 1); break;
		case 28: count_memcg_events(memcg, FRE28, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST28, 1); break;
		case 29: count_memcg_events(memcg, FRE29, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST29, 1); break;
		case 30: count_memcg_events(memcg, FRE30, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FAST30, 1); break;
		default: count_memcg_events(memcg, FREX, 1); if (info->prio >= 100) count_memcg_events(memcg, FRE_FASTX, 1); break;
	}
	// switch(dist){
	// 	case 0: 
	// 		if (info->prio >= 100){ // -2 1005
	// 			count_memcg_events(memcg ,WORKINGSET_REFAULT_FAST_DIST0, 1);
	// 		}
	// 		break;
	// 	case 1: 
	// 		count_memcg_events(memcg, WORKINGSET_REFAULT_DIST1, 1);
	// 		if (info->prio >= 100){ // -2 1005
	// 			count_memcg_events(memcg ,WORKINGSET_REFAULT_FAST_DIST1, 1);
	// 		}
	// 		break;
	// 	case 2: 
	// 	case 3: 
	// 		count_memcg_events(memcg, WORKINGSET_REFAULT_DIST2, 1);
	// 		if (info->prio >= 100){ // -2 1005
	// 			count_memcg_events(memcg ,WORKINGSET_REFAULT_FAST_DIST2, 1);
	// 		}
	// 		break;
	// 	case 4: 
	// 	case 5: 
	// 	case 6: 
	// 	case 7: 
	// 		count_memcg_events(memcg, WORKINGSET_REFAULT_DIST3, 1);
	// 		if (info->prio >= 100){ // -2 1005
	// 			count_memcg_events(memcg ,WORKINGSET_REFAULT_FAST_DIST3, 1);
	// 		}
	// 		break;
	// 	default:
	// 		count_memcg_events(memcg, WORKINGSET_REFAULT_DIST4, 1);
	// 		if (info->prio >= 100){ // -2 1005
	// 			count_memcg_events(memcg ,WORKINGSET_REFAULT_FAST_DIST4, 1);
	// 		}
	// 		break;
	// };
	// if (dist >= 20){ //its time to give up on this page
		// if (abandon_shadow)
		// 	*abandon_shadow = true;
	// }
skip_count:
	/*DJL ADD END*/
	/*DJL ADD BEGIN*/
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	if (shadow && (entry_is_entry_ext(shadow) == 1))
		trace_folio_ws_chg_se(folio, va, (unsigned short)memcg_id, token, refs, 1, swap_level, -2, shadow, entry);
	else
		trace_folio_ws_chg(folio, va,  pgdat, (unsigned short)memcg_id, token, refs, 1, swap_level, -2, entry);
#else
	trace_folio_ws_chg(folio, va,  pgdat, (unsigned short)memcg_id, token, refs, 1, swap_level, -2, entry);
#endif /*CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY*/
	/*DJL ADD END*/
#ifdef CONFIG_LRU_GEN_PASSIVE_SWAP_ALLOC
	// folio_set_swappriohigh(folio);
	
	// *try_free_entry = dist_ret;
	*try_free_entry = false;
#endif /*CONFIG_LRU_GEN_PASSIVE_SWAP_ALLOC*/
	// if (dist <= 3){
	// 	folio_swapprio_promote(folio);
	// 	if (folio_test_swappriolow(folio))
	// 		pr_err("folio[%p] promote fail", folio);		
	// }
	// else if (dist > 3){ //punishment
	// 	folio_set_swappriolow(folio);
	// 	folio_clear_swappriohigh(folio);
	// }
	/*DJL ADD END*/

	// if ((token >> LRU_REFS_WIDTH) != (min_seq & (EVICTION_MASK >> LRU_REFS_WIDTH)))
	// 	goto unlock;

	hist = lru_hist_from_seq(min_seq);
	/* see the comment in folio_lru_refs() */
	refs = (token & (BIT(LRU_REFS_WIDTH) - 1)) + workingset;
	tier = lru_tier_from_refs(refs);
	
	// /*DJL ADD BEGIN*/
	// trace_folio_ws_chg(folio, va, folio_test_swappriolow(folio), folio_test_swappriohigh(folio), pgdat, (unsigned short)memcg_id, token, refs, 1, swap_level);
	// /*DJL ADD END*/

	atomic_long_add(delta, &lrugen->refaulted[hist][type][tier]);
	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + type, delta);

	/*
	 * Count the following two cases as stalls:
	 * 1. For pages accessed through page tables, hotter pages pushed out
	 *    hot pages which refaulted immediately.
	 * 2. For pages accessed multiple times through file descriptors,
	 *    numbers of accesses might have been out of the range.
	 */
	if (lru_gen_in_fault() || refs == BIT(LRU_REFS_WIDTH)) {
		folio_set_workingset(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + type, delta);
	}

unlock:
	rcu_read_unlock();
}

#else /* !CONFIG_LRU_GEN */

static void *lru_gen_eviction(struct folio *folio, int swap_level, long swap_space_left)
{
	return NULL;
}

static void lru_gen_refault(struct folio *folio, void *shadow, int* try_free_entry
							int swap_level, unsigned long entry, bool* abandon_shadow)
{
}

#endif /* CONFIG_LRU_GEN */

/**
 * workingset_age_nonresident - age non-resident entries as LRU ages
 * @lruvec: the lruvec that was aged
 * @nr_pages: the number of pages to count
 *
 * As in-memory pages are aged, non-resident pages need to be aged as
 * well, in order for the refault distances later on to be comparable
 * to the in-memory dimensions. This function allows reclaim and LRU
 * operations to drive the non-resident aging along in parallel.
 */
void workingset_age_nonresident(struct lruvec *lruvec, unsigned long nr_pages)
{
	/*
	 * Reclaiming a cgroup means reclaiming all its children in a
	 * round-robin fashion. That means that each cgroup has an LRU
	 * order that is composed of the LRU orders of its child
	 * cgroups; and every page has an LRU position not just in the
	 * cgroup that owns it, but in all of that group's ancestors.
	 *
	 * So when the physical inactive list of a leaf cgroup ages,
	 * the virtual inactive lists of all its parents, including
	 * the root cgroup's, age as well.
	 */
	do {
		atomic_long_add(nr_pages, &lruvec->nonresident_age);
	} while ((lruvec = parent_lruvec(lruvec)));
}

/**
 * workingset_eviction - note the eviction of a folio from memory
 * @target_memcg: the cgroup that is causing the reclaim
 * @folio: the folio being evicted
 *
 * Return: a shadow entry to be stored in @folio->mapping->i_pages in place
 * of the evicted @folio so that a later refault can be detected.
 */
void *workingset_eviction(struct folio *folio, struct mem_cgroup *target_memcg, int swap_level, long swap_space_left, struct shadow_entry* se, swp_entry_t entry)
{
	struct pglist_data *pgdat = folio_pgdat(folio);
	unsigned long eviction;
	struct lruvec *lruvec;
	int memcgid;

	/* Folio is fully exclusive and pins folio's memory cgroup pointer */
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);
	VM_BUG_ON_FOLIO(folio_ref_count(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (lru_gen_enabled())
		return lru_gen_eviction(folio, swap_level , swap_space_left, se, entry);

	lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	/* XXX: target_memcg can be NULL, go through lruvec */
	memcgid = mem_cgroup_id(lruvec_memcg(lruvec));
	eviction = atomic_long_read(&lruvec->nonresident_age);
	eviction >>= bucket_order;
	workingset_age_nonresident(lruvec, folio_nr_pages(folio));
	return pack_shadow(memcgid, pgdat, eviction,
				folio_test_workingset(folio));
}

/**
 * workingset_refault - Evaluate the refault of a previously evicted folio.
 * @folio: The freshly allocated replacement folio.
 * @shadow: Shadow entry of the evicted folio.
 *
 * Calculates and evaluates the refault distance of the previously
 * evicted folio in the context of the node and the memcg whose memory
 * pressure caused the eviction.
 */
void workingset_refault(struct folio *folio, void *shadow, int* try_free_entry, 
						unsigned long va, int swap_level, swp_entry_t entry, bool* abandon_shadow)
{
	bool file = folio_is_file_lru(folio);
	struct mem_cgroup *eviction_memcg;
	struct lruvec *eviction_lruvec;
	unsigned long refault_distance;
	unsigned long workingset_size;
	struct pglist_data *pgdat;
	struct mem_cgroup *memcg;
	unsigned long eviction;
	struct lruvec *lruvec;
	unsigned long refault;
	bool workingset;
	int memcgid;
	long nr;

	if (lru_gen_enabled()) {
		lru_gen_refault(folio, shadow, try_free_entry, va, swap_level, (unsigned long)entry.val, abandon_shadow);
		return;
	}

	unpack_shadow(shadow, &memcgid, &pgdat, &eviction, &workingset);
	eviction <<= bucket_order;

	rcu_read_lock();
	/*
	 * Look up the memcg associated with the stored ID. It might
	 * have been deleted since the folio's eviction.
	 *
	 * Note that in rare events the ID could have been recycled
	 * for a new cgroup that refaults a shared folio. This is
	 * impossible to tell from the available data. However, this
	 * should be a rare and limited disturbance, and activations
	 * are always speculative anyway. Ultimately, it's the aging
	 * algorithm's job to shake out the minimum access frequency
	 * for the active cache.
	 *
	 * XXX: On !CONFIG_MEMCG, this will always return NULL; it
	 * would be better if the root_mem_cgroup existed in all
	 * configurations instead.
	 */
	eviction_memcg = mem_cgroup_from_id(memcgid);
	if (!mem_cgroup_disabled() && !eviction_memcg)
		goto out;
	eviction_lruvec = mem_cgroup_lruvec(eviction_memcg, pgdat);
	refault = atomic_long_read(&eviction_lruvec->nonresident_age);

	/*
	 * Calculate the refault distance
	 *
	 * The unsigned subtraction here gives an accurate distance
	 * across nonresident_age overflows in most cases. There is a
	 * special case: usually, shadow entries have a short lifetime
	 * and are either refaulted or reclaimed along with the inode
	 * before they get too old.  But it is not impossible for the
	 * nonresident_age to lap a shadow entry in the field, which
	 * can then result in a false small refault distance, leading
	 * to a false activation should this old entry actually
	 * refault again.  However, earlier kernels used to deactivate
	 * unconditionally with *every* reclaim invocation for the
	 * longest time, so the occasional inappropriate activation
	 * leading to pressure on the active list is not a problem.
	 */
	refault_distance = (refault - eviction) & EVICTION_MASK;

	/*
	 * The activation decision for this folio is made at the level
	 * where the eviction occurred, as that is where the LRU order
	 * during folio reclaim is being determined.
	 *
	 * However, the cgroup that will own the folio is the one that
	 * is actually experiencing the refault event.
	 */
	nr = folio_nr_pages(folio);
	memcg = folio_memcg(folio);
	pgdat = folio_pgdat(folio);
	lruvec = mem_cgroup_lruvec(memcg, pgdat);

	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + file, nr);

	mem_cgroup_flush_stats_delayed();
	/*
	 * Compare the distance to the existing workingset size. We
	 * don't activate pages that couldn't stay resident even if
	 * all the memory was available to the workingset. Whether
	 * workingset competition needs to consider anon or not depends
	 * on having swap.
	 */
	workingset_size = lruvec_page_state(eviction_lruvec, NR_ACTIVE_FILE);
	if (!file) {
		workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_INACTIVE_FILE);
	}
	if (mem_cgroup_get_nr_swap_pages(eviction_memcg) > 0) {
		workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_ACTIVE_ANON);
		if (file) {
			workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_INACTIVE_ANON);
		}
	}
	if (refault_distance > workingset_size)
		goto out;

	folio_set_active(folio);
	workingset_age_nonresident(lruvec, nr);
	mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + file, nr);

	/* Folio was active prior to eviction */
	if (workingset) {
		folio_set_workingset(folio);
		/*
		 * XXX: Move to folio_add_lru() when it supports new vs
		 * putback
		 */
		lru_note_cost_refault(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + file, nr);
	}
out:
	rcu_read_unlock();
	*try_free_entry = 0;
}

/**
 * workingset_activation - note a page activation
 * @folio: Folio that is being activated.
 */
void workingset_activation(struct folio *folio)
{
	struct mem_cgroup *memcg;

	rcu_read_lock();
	/*
	 * Filter non-memcg pages here, e.g. unmap can call
	 * mark_page_accessed() on VDSO pages.
	 *
	 * XXX: See workingset_refault() - this should return
	 * root_mem_cgroup even for !CONFIG_MEMCG.
	 */
	memcg = folio_memcg_rcu(folio);
	if (!mem_cgroup_disabled() && !memcg)
		goto out;
	workingset_age_nonresident(folio_lruvec(folio), folio_nr_pages(folio));
out:
	rcu_read_unlock();
}

/*
 * Shadow entries reflect the share of the working set that does not
 * fit into memory, so their number depends on the access pattern of
 * the workload.  In most cases, they will refault or get reclaimed
 * along with the inode, but a (malicious) workload that streams
 * through files with a total size several times that of available
 * memory, while preventing the inodes from being reclaimed, can
 * create excessive amounts of shadow nodes.  To keep a lid on this,
 * track shadow nodes and reclaim them when they grow way past the
 * point where they would still be useful.
 */

struct list_lru shadow_nodes;

void workingset_update_node(struct xa_node *node)
{
	struct address_space *mapping;

	/*
	 * Track non-empty nodes that contain only shadow entries;
	 * unlink those that contain pages or are being freed.
	 *
	 * Avoid acquiring the list_lru lock when the nodes are
	 * already where they should be. The list_empty() test is safe
	 * as node->private_list is protected by the i_pages lock.
	 */
	mapping = container_of(node->array, struct address_space, i_pages);
	lockdep_assert_held(&mapping->i_pages.xa_lock);

	if (node->count && node->count == node->nr_values) {
		if (list_empty(&node->private_list)) {
			list_lru_add(&shadow_nodes, &node->private_list);
			__inc_lruvec_kmem_state(node, WORKINGSET_NODES);
		}
	} else {
		if (!list_empty(&node->private_list)) {
			list_lru_del(&shadow_nodes, &node->private_list);
			__dec_lruvec_kmem_state(node, WORKINGSET_NODES);
		}
	}
}

static unsigned long count_shadow_nodes(struct shrinker *shrinker,
					struct shrink_control *sc)
{
	unsigned long max_nodes;
	unsigned long nodes;
	unsigned long pages;

	nodes = list_lru_shrink_count(&shadow_nodes, sc);
	if (!nodes)
		return SHRINK_EMPTY;

	/*
	 * Approximate a reasonable limit for the nodes
	 * containing shadow entries. We don't need to keep more
	 * shadow entries than possible pages on the active list,
	 * since refault distances bigger than that are dismissed.
	 *
	 * The size of the active list converges toward 100% of
	 * overall page cache as memory grows, with only a tiny
	 * inactive list. Assume the total cache size for that.
	 *
	 * Nodes might be sparsely populated, with only one shadow
	 * entry in the extreme case. Obviously, we cannot keep one
	 * node for every eligible shadow entry, so compromise on a
	 * worst-case density of 1/8th. Below that, not all eligible
	 * refaults can be detected anymore.
	 *
	 * On 64-bit with 7 xa_nodes per page and 64 slots
	 * each, this will reclaim shadow entries when they consume
	 * ~1.8% of available memory:
	 *
	 * PAGE_SIZE / xa_nodes / node_entries * 8 / PAGE_SIZE
	 */
#ifdef CONFIG_MEMCG
	if (sc->memcg) {
		struct lruvec *lruvec;
		int i;

		lruvec = mem_cgroup_lruvec(sc->memcg, NODE_DATA(sc->nid));
		for (pages = 0, i = 0; i < NR_LRU_LISTS; i++)
			pages += lruvec_page_state_local(lruvec,
							 NR_LRU_BASE + i);
		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_RECLAIMABLE_B) >> PAGE_SHIFT;
		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_UNRECLAIMABLE_B) >> PAGE_SHIFT;
	} else
#endif
		pages = node_present_pages(sc->nid);

	max_nodes = pages >> (XA_CHUNK_SHIFT - 3);

	if (nodes <= max_nodes)
		return 0;
	return nodes - max_nodes;
}

static enum lru_status shadow_lru_isolate(struct list_head *item,
					  struct list_lru_one *lru,
					  spinlock_t *lru_lock,
					  void *arg) __must_hold(lru_lock)
{
	struct xa_node *node = container_of(item, struct xa_node, private_list);
	struct address_space *mapping;
	int ret;

	/*
	 * Page cache insertions and deletions synchronously maintain
	 * the shadow node LRU under the i_pages lock and the
	 * lru_lock.  Because the page cache tree is emptied before
	 * the inode can be destroyed, holding the lru_lock pins any
	 * address_space that has nodes on the LRU.
	 *
	 * We can then safely transition to the i_pages lock to
	 * pin only the address_space of the particular node we want
	 * to reclaim, take the node off-LRU, and drop the lru_lock.
	 */

	mapping = container_of(node->array, struct address_space, i_pages);

	/* Coming from the list, invert the lock order */
	if (!xa_trylock(&mapping->i_pages)) {
		spin_unlock_irq(lru_lock);
		ret = LRU_RETRY;
		goto out;
	}

	/* For page cache we need to hold i_lock */
	if (mapping->host != NULL) {
		if (!spin_trylock(&mapping->host->i_lock)) {
			xa_unlock(&mapping->i_pages);
			spin_unlock_irq(lru_lock);
			ret = LRU_RETRY;
			goto out;
		}
	}

	list_lru_isolate(lru, item);
	__dec_lruvec_kmem_state(node, WORKINGSET_NODES);

	spin_unlock(lru_lock);

	/*
	 * The nodes should only contain one or more shadow entries,
	 * no pages, so we expect to be able to remove them all and
	 * delete and free the empty node afterwards.
	 */
	if (WARN_ON_ONCE(!node->nr_values))
		goto out_invalid;
	if (WARN_ON_ONCE(node->count != node->nr_values))
		goto out_invalid;

	/*
	 * Count timeout-based shadow entry releases before deleting the node.
	 * This is the REAL timeout release - shadow entries that haven't been
	 * refaulted for a long time and are being reclaimed due to memory pressure.
	 * 
	 * node->nr_values contains the number of shadow entries in this node.
	 * Since the comment states "nodes should only contain one or more shadow entries",
	 * we can use this count directly for our timeout statistics.
	 */
	{
		unsigned int shadow_entries_count = node->nr_values;
		int i;
		void *entry;
		
		/* Iterate through the node's slots to find and count shadow entries
		 * XA_CHUNK_SIZE is typically 64, but let's use a safe upper bound */
		for (i = 0; i < 64 && shadow_entries_count > 0; i++) {
			entry = node->slots[i];
			if (entry && !xa_is_node(entry)) {
				/* Check if this is our shadow entry extension */
				if (entry_is_entry_ext(entry) == 1) {
					/* This is a real timeout release - count it */
					count_shadow_timeout_event((struct shadow_entry *)entry);
					shadow_entries_count--;
				}
			}
		}
	}

	xa_delete_node(node, workingset_update_node);
	__inc_lruvec_kmem_state(node, WORKINGSET_NODERECLAIM);

out_invalid:
	xa_unlock_irq(&mapping->i_pages);
	if (mapping->host != NULL) {
		if (mapping_shrinkable(mapping))
			inode_add_lru(mapping->host);
		spin_unlock(&mapping->host->i_lock);
	}
	ret = LRU_REMOVED_RETRY;
out:
	cond_resched();
	spin_lock_irq(lru_lock);
	return ret;
}

static unsigned long scan_shadow_nodes(struct shrinker *shrinker,
				       struct shrink_control *sc)
{
	/* list_lru lock nests inside the IRQ-safe i_pages lock */
	return list_lru_shrink_walk_irq(&shadow_nodes, sc, shadow_lru_isolate,
					NULL);
}

static struct shrinker workingset_shadow_shrinker = {
	.count_objects = count_shadow_nodes,
	.scan_objects = scan_shadow_nodes,
	.seeks = 0, /* ->count reports only fully expendable nodes */
	.flags = SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE,
};

/*
 * Our list_lru->lock is IRQ-safe as it nests inside the IRQ-safe
 * i_pages lock.
 */
static struct lock_class_key shadow_nodes_key;

static void shadow_entry_ctor(void *data)
{
}

static int shadow_entry_cache_init(void)
{
	shadow_entry_cache = kmem_cache_create("shadow_entry", sizeof(struct shadow_entry),
			0 , SLAB_PANIC, //|SLAB_ACCOUNT,//SLAB_TYPESAFE_BY_RCU
			shadow_entry_ctor);
	if (shadow_entry_cache)
		return 0;
	return -1;
}

static int __init workingset_init(void)
{
	unsigned int timestamp_bits;
	unsigned int max_order;
	int ret;

	BUILD_BUG_ON(BITS_PER_LONG < EVICTION_SHIFT);
	/*
	 * Calculate the eviction bucket size to cover the longest
	 * actionable refault distance, which is currently half of
	 * memory (totalram_pages/2). However, memory hotplug may add
	 * some more pages at runtime, so keep working with up to
	 * double the initial memory by using totalram_pages as-is.
	 */
	timestamp_bits = BITS_PER_LONG - EVICTION_SHIFT;
	max_order = fls_long(totalram_pages() - 1);
	if (max_order > timestamp_bits)
		bucket_order = max_order - timestamp_bits;
	pr_info("workingset: timestamp_bits=%d max_order=%d bucket_order=%u\n",
	       timestamp_bits, max_order, bucket_order);

	ret = prealloc_shrinker(&workingset_shadow_shrinker, "mm-shadow");
	if (ret)
		goto err;
	ret = __list_lru_init(&shadow_nodes, true, &shadow_nodes_key,
			      &workingset_shadow_shrinker);
	if (ret)
		goto err_list_lru;
	register_shrinker_prepared(&workingset_shadow_shrinker);
	shadow_entry_cache_init();
	return 0;
err_list_lru:
	free_prealloced_shrinker(&workingset_shadow_shrinker);
err:
	return ret;
}
module_init(workingset_init);
