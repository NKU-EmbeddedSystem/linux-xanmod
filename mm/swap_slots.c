// SPDX-License-Identifier: GPL-2.0
/*
 * Manage cache of swap slots to be used for and returned from
 * swap.
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * Author: Tim Chen <tim.c.chen@linux.intel.com>
 *
 * We allocate the swap slots from the global pool and put
 * it into local per cpu caches.  This has the advantage
 * of no needing to acquire the swap_info lock every time
 * we need a new slot.
 *
 * There is also opportunity to simply return the slot
 * to local caches without needing to acquire swap_info
 * lock.  We do not reuse the returned slots directly but
 * move them back to the global pool in a batch.  This
 * allows the slots to coalesce and reduce fragmentation.
 *
 * The swap entry allocated is marked with SWAP_HAS_CACHE
 * flag in map_count that prevents it from being allocated
 * again from the global pool.
 *
 * The swap slots cache is protected by a mutex instead of
 * a spin lock as when we search for slots with scan_swap_map,
 * we can possibly sleep.
 */

#include <linux/swap_slots.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/memcontrol.h>
#include <trace/events/lru_gen.h>

static DEFINE_PER_CPU(struct swap_slots_cache, swp_slots);
static bool	swap_slot_cache_active;
bool	swap_slot_cache_enabled;
static bool	swap_slot_cache_initialized;
static DEFINE_MUTEX(swap_slots_cache_mutex);
/* Serialize swap slots cache enable/disable operations */
static DEFINE_MUTEX(swap_slots_cache_enable_mutex);

static void __drain_swap_slots_cache(unsigned int type);

#define use_swap_slot_cache (swap_slot_cache_active && swap_slot_cache_enabled)
#define SLOTS_CACHE 0x1
#define SLOTS_CACHE_RET 0x2
/*DJL ADD BEGIN*/
#define SLOTS_CACHE_SLOW 0x4
#define SLOTS_CACHE_FAST 0x8
/*DJL ADD END*/
// extern signed short fastest_swap_prio;
// extern signed short slowest_swap_prio;

static void deactivate_swap_slots_cache(void)
{
	mutex_lock(&swap_slots_cache_mutex);
	swap_slot_cache_active = false;
	__drain_swap_slots_cache(SLOTS_CACHE|SLOTS_CACHE_RET|SLOTS_CACHE_FAST|SLOTS_CACHE_SLOW);
	mutex_unlock(&swap_slots_cache_mutex);
}

static void reactivate_swap_slots_cache(void)
{
	mutex_lock(&swap_slots_cache_mutex);
	swap_slot_cache_active = true;
	mutex_unlock(&swap_slots_cache_mutex);
}

/* Must not be called with cpu hot plug lock */
void disable_swap_slots_cache_lock(void)
{
	mutex_lock(&swap_slots_cache_enable_mutex);
	swap_slot_cache_enabled = false;
	if (swap_slot_cache_initialized) {
		/* serialize with cpu hotplug operations */
		cpus_read_lock();
		__drain_swap_slots_cache(SLOTS_CACHE|SLOTS_CACHE_RET|SLOTS_CACHE_FAST|SLOTS_CACHE_SLOW);
		cpus_read_unlock();
	}
}

static void __reenable_swap_slots_cache(void)
{
	swap_slot_cache_enabled = has_usable_swap();
}

void reenable_swap_slots_cache_unlock(void)
{
	__reenable_swap_slots_cache();
	mutex_unlock(&swap_slots_cache_enable_mutex);
}

static bool check_cache_active(void)
{
	long pages;

	if (!swap_slot_cache_enabled)
		return false;

	pages = get_nr_swap_pages();
	if (!swap_slot_cache_active) {
		if (pages > num_online_cpus() *
		    THRESHOLD_ACTIVATE_SWAP_SLOTS_CACHE)
			reactivate_swap_slots_cache();
		goto out;
	}

	/* if global pool of slot caches too low, deactivate cache */
	if (pages < num_online_cpus() * THRESHOLD_DEACTIVATE_SWAP_SLOTS_CACHE)
		deactivate_swap_slots_cache();
out:
	return swap_slot_cache_active;
}

static int alloc_swap_slot_cache(unsigned int cpu)
{
	struct swap_slots_cache *cache;
	swp_entry_t *slots, *slots_ret;
	swp_entry_t *slots_slow, *slots_fast;

	/*
	 * Do allocation outside swap_slots_cache_mutex
	 * as kvzalloc could trigger reclaim and folio_alloc_swap,
	 * which can lock swap_slots_cache_mutex.
	 */
	slots = kvcalloc(SWAP_SLOTS_CACHE_SIZE, sizeof(swp_entry_t),
			 GFP_KERNEL);
	if (!slots)
		return -ENOMEM;

	slots_ret = kvcalloc(SWAP_SLOTS_CACHE_SIZE, sizeof(swp_entry_t),
			     GFP_KERNEL);
	if (!slots_ret) {
		kvfree(slots);
		return -ENOMEM;
	}
	/*DJL ADD BEGIN*/
	slots_slow = kvcalloc(SWAP_SLOTS_CACHE_SIZE, sizeof(swp_entry_t),
				 GFP_KERNEL);
	if (!slots_slow){
		kvfree(slots);
		kvfree(slots_ret);
		return -ENOMEM;
	}
	slots_fast = kvcalloc(SWAP_SLOTS_CACHE_SIZE, sizeof(swp_entry_t),
				 GFP_KERNEL);
	if (!slots_fast){
		kvfree(slots);
		kvfree(slots_ret);
		kvfree(slots_slow);
		return -ENOMEM;
	}
	/*DJL ADD END*/
	mutex_lock(&swap_slots_cache_mutex);
	cache = &per_cpu(swp_slots, cpu);
	if (cache->slots || cache->slots_ret) {
		/* cache already allocated */
		mutex_unlock(&swap_slots_cache_mutex);

		kvfree(slots);
		kvfree(slots_ret);
		kvfree(slots_slow);
		kvfree(slots_fast);

		return 0;
	}

	if (!cache->lock_initialized) {
		mutex_init(&cache->alloc_lock);
		spin_lock_init(&cache->free_lock);
		cache->lock_initialized = true;
	}
	cache->nr = 0;
	cache->cur = 0;
	cache->n_ret = 0;
	/*DJL ADD BEGIN*/
	cache->cur_fast = 0;
	cache->nr_fast = 0;
	cache->cur_slow = 0;
	cache->nr_slow = 0;
	/*DJL ADD END*/
	/*
	 * We initialized alloc_lock and free_lock earlier.  We use
	 * !cache->slots or !cache->slots_ret to know if it is safe to acquire
	 * the corresponding lock and use the cache.  Memory barrier below
	 * ensures the assumption.
	 */
	mb();
	cache->slots = slots;
	cache->slots_ret = slots_ret;
	/*DJL ADD BEGIN*/
	cache->slots_fast = slots_fast;
	cache->slots_slow = slots_slow;
	cache->fast_left = cache->slow_left = cache->left = 99999;
	/*DJL ADD END*/

	mutex_unlock(&swap_slots_cache_mutex);
	return 0;
}

static void drain_slots_cache_cpu(unsigned int cpu, unsigned int type,
				  bool free_slots)
{
	struct swap_slots_cache *cache;
	swp_entry_t *slots = NULL;

	cache = &per_cpu(swp_slots, cpu);
	if ((type & SLOTS_CACHE) && cache->slots) {
		mutex_lock(&cache->alloc_lock);
		swapcache_free_entries(cache->slots + cache->cur, cache->nr, 1);
		cache->cur = 0;
		cache->nr = 0;
		if (free_slots && cache->slots) {
			kvfree(cache->slots);
			cache->slots = NULL;
		}
		mutex_unlock(&cache->alloc_lock);
	}
	if ((type & SLOTS_CACHE_FAST) && cache->slots_fast) {
		mutex_lock(&cache->alloc_lock);
		swapcache_free_entries(cache->slots_fast + cache->cur_fast, cache->nr_fast, 1);
		cache->cur_fast = 0;
		cache->nr_fast = 0;
		if (free_slots && cache->slots_fast) {
			kvfree(cache->slots_fast);
			cache->slots_fast = NULL;
		}
		mutex_unlock(&cache->alloc_lock);
	}
	if ((type & SLOTS_CACHE_SLOW) && cache->slots_slow) {
		mutex_lock(&cache->alloc_lock);
		swapcache_free_entries(cache->slots_slow + cache->cur_slow, cache->nr_slow, 1);
		cache->cur_slow = 0;
		cache->nr_slow = 0;
		if (free_slots && cache->slots_slow) {
			kvfree(cache->slots_slow);
			cache->slots_slow = NULL;
		}
		mutex_unlock(&cache->alloc_lock);
	}
	if ((type & SLOTS_CACHE_RET) && cache->slots_ret) {
		spin_lock_irq(&cache->free_lock);
		swapcache_free_entries(cache->slots_ret, cache->n_ret, 1);
		cache->n_ret = 0;
		if (free_slots && cache->slots_ret) {
			slots = cache->slots_ret;
			cache->slots_ret = NULL;
		}
		spin_unlock_irq(&cache->free_lock);
		kvfree(slots);
	}
}

static void __drain_swap_slots_cache(unsigned int type)
{
	unsigned int cpu;

	/*
	 * This function is called during
	 *	1) swapoff, when we have to make sure no
	 *	   left over slots are in cache when we remove
	 *	   a swap device;
	 *      2) disabling of swap slot cache, when we run low
	 *	   on swap slots when allocating memory and need
	 *	   to return swap slots to global pool.
	 *
	 * We cannot acquire cpu hot plug lock here as
	 * this function can be invoked in the cpu
	 * hot plug path:
	 * cpu_up -> lock cpu_hotplug -> cpu hotplug state callback
	 *   -> memory allocation -> direct reclaim -> folio_alloc_swap
	 *   -> drain_swap_slots_cache
	 *
	 * Hence the loop over current online cpu below could miss cpu that
	 * is being brought online but not yet marked as online.
	 * That is okay as we do not schedule and run anything on a
	 * cpu before it has been marked online. Hence, we will not
	 * fill any swap slots in slots cache of such cpu.
	 * There are no slots on such cpu that need to be drained.
	 */
	for_each_online_cpu(cpu)
		drain_slots_cache_cpu(cpu, type, false);
}

static int free_slot_cache(unsigned int cpu)
{
	mutex_lock(&swap_slots_cache_mutex);
	drain_slots_cache_cpu(cpu, SLOTS_CACHE | SLOTS_CACHE_RET |SLOTS_CACHE_FAST|SLOTS_CACHE_SLOW, true);
	mutex_unlock(&swap_slots_cache_mutex);
	return 0;
}

void enable_swap_slots_cache(void)
{
	mutex_lock(&swap_slots_cache_enable_mutex);
	if (!swap_slot_cache_initialized) {
		int ret;

		ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "swap_slots_cache",
					alloc_swap_slot_cache, free_slot_cache);
		if (WARN_ONCE(ret < 0, "Cache allocation failed (%s), operating "
				       "without swap slots cache.\n", __func__))
			goto out_unlock;

		swap_slot_cache_initialized = true;
	}

	__reenable_swap_slots_cache();
out_unlock:
	mutex_unlock(&swap_slots_cache_enable_mutex);
}

/*
 * MULTISWAP: 
 * refill swap slotes cache using swap at cache->prio, 
 * then update swap slot cache's stat. 
 * - called with swap slot cache's alloc lock held
 */
static int refill_swap_slots_cache(struct swap_slots_cache *cache)
{
	if (!use_swap_slot_cache)
		return 0;

	cache->cur = 0;
	if (swap_slot_cache_active)
		cache->nr = get_swap_pages(SWAP_SLOTS_CACHE_SIZE,
					   cache->slots, 1, 0, &cache->prio, &cache->left);
	// if (fastest_swap_prio < cache->prio) fastest_swap_prio = cache->prio;
	// if (slowest_swap_prio > cache->prio) slowest_swap_prio = cache->prio;
	if (get_fastest_swap_prio() == cache->prio) {cache->fast_left = cache->left;} else {cache->fast_left = 0;}
	trace_refill_swap_slots(0, cache->nr, cache->prio);
	return cache->nr;
}

/*
 * MULTISWAP: 
 * refill swap slotes cache using swap at cache->prio_slow, 
 * then update swap slot cache's stat. 
 * - called with swap slot cache's alloc lock held
 */
 static int refill_swap_slots_slow_cache(struct swap_slots_cache *cache)
{
	if (!use_swap_slot_cache)
		return 0;

	cache->cur_slow = 0;
	if (swap_slot_cache_active)
		cache->nr_slow = get_swap_pages(SWAP_SLOTS_CACHE_SIZE,
					   cache->slots_slow, 1, 1, &cache->prio_slow, &cache->slow_left);	//DJL ADD PARAMETER
	// if (slowest_swap_prio > cache->prio_slow) slowest_swap_prio = cache->prio_slow;
	trace_refill_swap_slots(1, cache->nr_slow, cache->prio_slow);
	return cache->nr_slow;
}

/*
 * MULTISWAP: 
 * refill swap slotes cache using swap at cache->prio_fast, 
 * then update swap slot cache's stat. 
 * - called with swap slot cache's alloc lock held
 */
static int refill_swap_slots_fast_cache(struct swap_slots_cache *cache)
{
	long left = 0;
	if (!use_swap_slot_cache)
		return 0;

	cache->cur_fast = 0;
	if (swap_slot_cache_active)
		cache->nr_fast = get_swap_pages(SWAP_SLOTS_CACHE_SIZE,
					   cache->slots_fast, 1, 2, &cache->prio_fast, &left);//&cache->fast_left);	//DJL ADD PARAMETER

	if (cache->prio_fast == get_fastest_swap_prio())
		cache->fast_left = left;
	else
		cache->fast_left = 0;
	if (cache->prio_fast == get_fastest_swap_prio() && 
			cache->nr_fast && (0 == cache->fast_left)) //prio_fast didn't change && prio fast has left
	{
		pr_err("get_swap_pages inner err nrfast[%d] fastleft[%ld]", cache->nr_fast ,cache->fast_left);
		BUG();
	}
	trace_refill_swap_slots(2, cache->nr_fast, cache->prio_fast);
	return cache->nr_fast;
}

void free_swap_slot(swp_entry_t entry)
{
	struct swap_slots_cache *cache;

	cache = raw_cpu_ptr(&swp_slots);
	if (likely(use_swap_slot_cache && cache->slots_ret)) {
		spin_lock_irq(&cache->free_lock);
		/* Swap slots cache may be deactivated before acquiring lock */
		if (!use_swap_slot_cache || !cache->slots_ret) {
			spin_unlock_irq(&cache->free_lock);
			goto direct_free;
		}
		if (cache->n_ret >= SWAP_SLOTS_CACHE_SIZE) {
			/*
			 * Return slots to global pool.
			 * The current swap_map value is SWAP_HAS_CACHE.
			 * Set it to 0 to indicate it is available for
			 * allocation in global pool
			 */
			swapcache_free_entries(cache->slots_ret, cache->n_ret, 1);
			cache->n_ret = 0;
		}
		cache->slots_ret[cache->n_ret++] = entry;
		spin_unlock_irq(&cache->free_lock);
	} else {
direct_free:
		swapcache_free_entries(&entry, 1, 1);
	}
}

int is_first = 1;
extern unsigned int clever_swap_alloc;
swp_entry_t folio_alloc_swap(struct folio *folio, long* left_space, bool force_slow, bool skip_charge)
{
	swp_entry_t entry;
	struct swap_slots_cache *cache;
	/*DJL ADD BEGIN*/
	unsigned short prio;
	int _nr, _cur, _prio;
	swp_entry_t* _slots;
	int swap_router_result;
	long fast_left;
	struct shadow_entry* shadow_ext;
	/*DJL ADD END*/
	
	entry.val = 0;
	if (left_space)
		*left_space = -3;

	if (folio_test_large(folio)) {
		if (IS_ENABLED(CONFIG_THP_SWAP) && arch_thp_swp_supported())
			get_swap_pages(1, &entry, folio_nr_pages(folio), 0, &prio, NULL);
		if (entry.val){
			count_memcg_folio_events(folio, SWAPOUT_RAW, folio_nr_pages(folio));
		}
		goto out;
	}

	/*
	 * Preemption is allowed here, because we may sleep
	 * in refill_swap_slots_cache().  But it is safe, because
	 * accesses to the per-CPU data structure are protected by the
	 * mutex cache->alloc_lock.
	 *
	 * The alloc path here does not touch cache->slots_ret
	 * so cache->free_lock is not taken.
	 */
	cache = raw_cpu_ptr(&swp_slots);
	/*DJL ADD BEGIN*/
	//shouldn't up together
	WARN_ON_ONCE(folio_test_swappriolow(folio) && folio_test_swappriohigh(folio));
	/*DJL ADD END*/
#ifdef CONFIG_LRU_GEN_SWAP_ROUTER
	fast_left = max(cache->fast_left, (long)0);
	if (entry_is_entry_ext(folio->shadow_ext) == 1){
		unsigned int refault_count, avg_distance;
		struct mem_cgroup *memcg;

		shadow_ext = (struct shadow_entry*)folio->shadow_ext;
		refault_count = shadow_ext->hist_ts[SE_HIST_REFAULT_COUNT];
		avg_distance = shadow_ext->hist_ts[SE_HIST_AVG_DISTANCE];

		/* Use average distance for decision making instead of generation values */

		if (refault_count > 0 && avg_distance < SE_HIST_INITIAL_AVG_DIST * SE_HIST_SCALE_FACTOR)
			count_memcg_folio_events(folio, LEAF7, 1);
		else if (refault_count == 0)
			count_memcg_folio_events(folio, LEAF6, 1);

		/* Use the new simple swap router decision function */
		memcg = folio_memcg(folio);
		if (memcg) {
			struct mem_cgroup_per_node *pn = memcg->nodeinfo[folio_nid(folio)];
			swap_router_result = swap_router_decision(refault_count, avg_distance,
								 &pn->lruvec.router_params);
		} else {
			/* No memcg context, use conservative default */
			swap_router_result = (refault_count > 0) ? 1 : 0;
		}
		count_memcg_folio_events(folio, WI_TREE, 1);
	}else{
		/* No shadow extension, conservative default */
		swap_router_result = 1;
		count_memcg_folio_events(folio, WO_TREE, 1);
	}
#endif
#ifdef CONFIG_LRU_GEN_SWAP_ROUTER
	//translate from folio_prio to swap_router_result, because its force
	// if (folio_test_swappriohigh(folio))
	// 	swap_router_result = 1;
	// else if (folio_test_swappriolow(folio))
	// 	swap_router_result = 0;
	// //stale-saved page force goto slow
	if (!clever_swap_alloc)
		swap_router_result = 1;
	if (force_slow)
		swap_router_result = 0;
	if (swap_router_result == 0){
#else
	if (folio_test_swappriolow(folio)){
#endif
		if (likely(check_cache_active() && cache->slots_slow)) {
			mutex_lock(&cache->alloc_lock);
			if (cache->slots_slow) {
repeat_slow:
				if (cache->nr_slow) {
					entry = cache->slots_slow[cache->cur_slow];
					cache->slots_slow[cache->cur_slow++].val = 0;
					cache->nr_slow--;
					if (left_space)*left_space = cache->slow_left;
				} else if (refill_swap_slots_slow_cache(cache)) {
					if (cache->prio == cache->prio_slow)
						cache->left = cache->slow_left;
					if (cache->prio_fast == cache->prio_slow)
						cache->fast_left = cache->slow_left;
					goto repeat_slow;
				}
			}
			mutex_unlock(&cache->alloc_lock);
			if (entry.val){
				count_memcg_folio_events(folio, SWAPOUT_SLOW_ASSIGN_SUCC,1);
				count_memcg_folio_events(folio, SWAPOUT_SLOW_ASSIGN_ATT,1);
				goto out;
			}
		}	
	}
#ifdef CONFIG_LRU_GEN_SWAP_ROUTER
	else if (swap_router_result == 1){
#else
	else if (folio_test_swappriohigh(folio)){//fast
#endif
		if (likely(check_cache_active() && cache->slots_fast)) {
			mutex_lock(&cache->alloc_lock);
			if (cache->slots_fast) {
repeat_fast:
				//if slower than normal , exchange
				if (cache->prio > cache->prio_fast){
					_nr = cache->nr;   cache->nr = cache->nr_fast;	  cache->nr_fast = _nr;
					_cur = cache->cur; cache->cur = cache->cur_fast;  cache->cur_fast = _cur;
					_slots = cache->slots; cache->slots = cache->slots_fast; cache->slots_fast = _slots;
					_prio = cache->prio;   cache->prio = cache->prio_fast;   cache->prio_fast = _prio;
				}				
				if (cache->nr_fast) {
					entry = cache->slots_fast[cache->cur_fast];
					cache->slots_fast[cache->cur_fast++].val = 0;
					cache->nr_fast--;
					if (left_space) *left_space = cache->fast_left;
				} else if (refill_swap_slots_fast_cache(cache)) {
					if (cache->prio == cache->prio_fast)
						cache->left = cache->fast_left;
					if (cache->prio_slow == cache->prio_fast)
						cache->slow_left = cache->fast_left;
					goto repeat_fast;
				}
			}
			mutex_unlock(&cache->alloc_lock);
			if (entry.val){
				if (cache->prio_fast == get_fastest_swap_prio())
					count_memcg_folio_events(folio, SWAPOUT_FAST_ASSIGN_SUCC, 1);
				else
					count_memcg_folio_events(folio, SWAPOUT_FAST_ASSIGN_FAIL, 1);
				count_memcg_folio_events(folio, SWAPOUT_FAST_ASSIGN_ATT,1);
				goto out;
			}
		}
	}
	else if (likely(check_cache_active() && cache->slots)) {
		mutex_lock(&cache->alloc_lock);
		if (cache->slots) {
repeat:
			if (cache->nr) {
				entry = cache->slots[cache->cur];
				cache->slots[cache->cur++].val = 0;
				cache->nr--;
				if (left_space) *left_space = cache->left;
			} else if (refill_swap_slots_cache(cache)) {
				if (cache->prio_fast == cache->prio)
					cache->fast_left = cache->left;
				if (cache->prio_slow == cache->prio)
					cache->slow_left = cache->left;
				goto repeat;
			}
		}
		mutex_unlock(&cache->alloc_lock);
		if (entry.val){
			if (cache->prio == get_fastest_swap_prio())
				count_memcg_folio_events(folio, SWAPOUT_FAST_ASSIGN_SUCC, 1);
			else if (cache->prio == get_slowest_swap_prio())
				count_memcg_folio_events(folio, SWAPOUT_SLOW_ASSIGN_SUCC, 1);
			else 
				count_memcg_folio_events(folio, SWAPOUT_MID_ASSIGN_SUCC, 1);
			count_memcg_folio_events(folio, SWAPOUT_MID_ASSIGN_ATT,1);
			goto out;
		}
	}

	get_swap_pages(1, &entry, 1, 0, &prio, left_space);
	if (entry.val){
		count_memcg_folio_events(folio, SWAPOUT_RAW, 1);
 	}
out:
	if (likely(!skip_charge)){
		if (mem_cgroup_try_charge_swap(folio, entry)) {
			put_swap_folio(folio, entry);
			entry.val = 0;
		}
	}
	return entry;
}
