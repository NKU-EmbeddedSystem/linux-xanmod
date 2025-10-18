// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/swap_scan_slot.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"
#include "swap.h"
/*DJL ADD START*/
#include <linux/memcontrol.h>

#include <trace/events/lru_gen.h>

#define CREATE_TRACE_POINTS
#include <trace/events/swap.h>
/*DJL ADD END*/
/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_page_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};

struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;

struct address_space *swapper_spaces_remap[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces_remap[MAX_SWAPFILES] __read_mostly;

static bool enable_vma_readahead __read_mostly = true;
/*DJL ADD BEGIN*/
static bool enable_vma_readahead_boost __read_mostly = false;//controller of boost
static bool enable_ra_fast_evict __read_mostly = false;//controller of boost
bool swap_scan_enabled_sysfs __read_mostly = true;
/*DJL ADD END*/

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);
/*DJL ADD BEGIN*/
#ifdef CONFIG_LRU_GEN_CGROUP_KSWAPD_BOOST
static int force_wake_up_delay = 10;
static int force_wake_up_delay_now = 0;
#endif
/*DJL ADD END*/
void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Free swap  = %ldkB\n",
		get_nr_swap_pages() << (PAGE_SHIFT - 10));
	printk("Total swap = %lukB\n", total_swap_pages << (PAGE_SHIFT - 10));
}

void *get_shadow_from_swap_cache(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	struct page *page;

	page = xa_load(&address_space->i_pages, idx);
	if (!page)
		return page;
	if (xa_is_value(page))
		return page;
	else if (entry_is_entry_ext(page)== 1)
		return page;
	else if (entry_is_entry_ext(page) == -1){
		pr_err("get_shadow_from_swap_cache entry[%lx] a freed page[%p]", entry.val, page);
	}
	return NULL;
}

void *get_shadow_from_swap_cache_erase(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, 0);
	unsigned long i;
	int entry_state = 0;
	void *old;
	
	/* DEBUG: Add unconditional trace to confirm function is called */
	// printk(KERN_ERR "DEBUG_SHADOW: ENTRY get_shadow_from_swap_cache_erase called for entry[%lx]\n", entry.val);
	
	xas_set_update(&xas, workingset_update_node);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < 1; i++) {
			old = xas_store(&xas, NULL);
//			pr_err("addr_space[%p]ind[%lx] = [%lx]", address_space, idx, old);
			if (old){
				if (xa_is_value(old)) { //files
					// xas_store(&xas, NULL);
					// pr_info("get shadow clean shadow entry[%lx]->shadow[%p]", entry.val, old);
					// printk(KERN_ERR "DEBUG_SHADOW: entry[%lx] file_shadow_entry - found file shadow entry\n", entry.val);
				}
				else{
					entry_state = entry_is_entry_ext(old);
					trace_get_shadow_swcache(entry.val, old, entry_state);

					if (entry_state > 0){ //shadow, erased now
						//pass
						// pr_info("get shadow clean shadow_ext entry[%lx]->shadow_ext[%lx]", entry.val, old);
						// printk(KERN_ERR "DEBUG_SHADOW: entry[%lx] shadow_found_success - successfully retrieved shadow entry\n", entry.val);
					}
					else if (-1 == entry_state){ //might be still in swapcache ?
						pr_err("return a freed entry[%lx]->shadow[%lx]", entry.val, old);
						// printk(KERN_ERR "DEBUG_SHADOW: entry[%lx] freed_shadow_entry - shadow entry already freed\n", entry.val);
						BUG();
						old = NULL;
					}
					else if (0 == entry_state){
						pr_err("get_shadow_from_s$ delete origin folio entry[%lx]->folio[%p]", entry.val, old);
						// printk(KERN_ERR "DEBUG_SHADOW: entry[%lx] not_shadow_but_folio - found folio instead of shadow\n", entry.val);
						xas_store(&xas, old);
						old = NULL;
					}
					else{
						BUG();
					}	
				}
			}
			else{
				//pass
				// pr_info("get shadow clean shadow_ext entry[%lx]->shadow_ext[%lx]", entry.val, old);
			}
			break;
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, GFP_KERNEL));
	if (!xas_error(&xas)) {
		/* DEBUG: Log the specific reason for NULL returns to understand the problem */
		if (old == NULL) {
			// printk(KERN_ERR "DEBUG_SHADOW: entry[%lx] swap_cache_empty - no shadow entry found\n", entry.val);
		}
		return old;
	}
	BUG();
	return NULL;
}

/*
 * add_to_swap_cache resembles filemap_add_folio on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache(struct folio *folio, swp_entry_t entry,
			gfp_t gfp, void **shadowp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);
	int entry_state = 0;
	void *old;

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	if (entry_is_entry_ext(folio) != 0){
		pr_err("add_to_swap_cache bad folio[%p]", folio);
		BUG();
	}
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			old = xas_load(&xas);
			if (old){
				if (xa_is_value(old)) { //files
					if (shadowp)
						*shadowp = old;
				}
				else{
					entry_state = entry_is_entry_ext(old);//entry_is_entry_ext_debug(old);
					if (entry_state == 1){ //swap
						if (shadowp){
							*shadowp = old;
						}else{ //
							pr_info("[FREE]folio[%p] add to sw$ , not freed entry[%lx] ext[%p]stale[%d] idx[%ld]", 
										folio, entry.val, old, folio_test_stalesaved(folio), idx);
							// trace_shadow_entry_free(old, 1);
							// shadow_entry_free(old);
							// BUG();
						}
					}
					else if (old && 0 == entry_state){
						struct folio* migrating_folio = (struct folio*)old;
						pr_info("return a folio entry[%lx]->folio[%p]st[%d], folio[%p] failed add $", 
									entry.val, old, folio_test_stalesaved(old), folio);
						// if (shadowp && migrating_folio->shadow_ext){
						// 	*shadowp = folio_remove_shadow_entry(migrating_folio);
						// 	BUG();
						// }
						// else{
						// 	shadow_entry_free(folio_remove_shadow_entry(migrating_folio));
						// }
						shadow_entry_free(folio_remove_shadow_entry(migrating_folio));
					}
					else if (-1 == entry_state){
						pr_err("add_to_swap_cache invalid entry[%lx]->shadow[%p], folio[%p] failed add $", entry.val, old, folio);
						BUG();
					}
					else{
						pr_err("add_to_swap_cache NULL ext entry[%lx]->shadow[%p], folio[%p] failed add $", entry.val, old, folio);
						BUG(); // NULL
					}
				}
			}else{
				if (shadowp)
					*shadowp = NULL;
			}

			// set_page_private_debug(folio_page(folio, i), entry.val + i, 1);
			set_page_private(folio_page(folio, i), entry.val + i);
			xas_store(&xas, folio);

			xas_next(&xas);
		}
		address_space->nrpages += nr;
		__node_stat_mod_folio(folio, NR_FILE_PAGES, nr);
		__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr);

#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
		/* Mark shadow_ext as entering eviction race window if it exists
		 * Race window: folio now in swap cache, but shadow_ext update still pending
		 * If refault happens before workingset_eviction() completes, we'll detect it */
		if (folio->shadow_ext && entry_is_entry_ext(folio->shadow_ext) == 1) {
			struct shadow_entry* shadow_ext = folio->shadow_ext;
#if !SE_HIST_USE_PAGE_ID
			shadow_ext->hist_ts[SE_HIST_EVICTION_RACE_STATE] = SE_RACE_STATE_EVICTING;
#endif
		}
#endif
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));
	// pr_err("set entry[%lx]->folio[%p]", entry.val, folio);
	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	folio_ref_sub(folio, nr);
	if (folio_test_swappriohigh(folio)){
		pr_err("add_to_swap_cache folio[%p] ref[%d] subbed", folio, folio_ref_count(folio));
		dump_stack();
	}
	return xas_error(&xas);
}

/*THIS ONLY MASK FOR DIFFERENT MAPS, DOESN"T DEAL WITH EXT BITS*/
int prepare_swp_entry(swp_entry_t from_entry, swp_entry_t* masked_entry)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	swp_entry_t mig_swap, from_entry_2;
	int current_offset = 0;

	mig_swap.val = 0;
	if (!address_space) 
		return -1;	
	
	xa_lock_irq(&address_space->i_pages);
	while (!(mig_swap.val)){
		pgoff_t idx;
		from_entry_2.val = from_entry.val;
		swp_entry_set_special(&from_entry_2, current_offset); //set offset
		idx = swp_offset(from_entry_2);
		XA_STATE(xas, &address_space->i_pages, idx);
		void *entry = xas_load(&xas);
		if (!entry){
			break;
		}
		mig_swap.val = xa_to_value(entry);
		if (mig_swap.val) { //conflict
			pr_err("from_swap conflict entry[%lx], cur[%d]", 
						from_entry_2.val,  current_offset);
			current_offset++;
			if (current_offset > SWP_MIG_MAX_MAP){
				pr_err("prepare_swp_entry ecceed error");
				xa_unlock_irq(&address_space->i_pages);
				return -2;
			}
			mig_swap.val = 0; //continue check
		}
	}	
	xa_unlock_irq(&address_space->i_pages);
	masked_entry->val = from_entry_2.val;
	pr_err("entry[%lx]=>[%lx] was saved [%d] time"
			, from_entry.val, from_entry_2.val, current_offset);

	return 0;
}

int enable_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t* p_to_entry)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	swp_entry_t to_entry, to_entry_enabled;
	long nr, i;
	void* entry;
	bool locked = false;
	pgoff_t idx = swp_offset(from_entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));

	VM_WARN_ON_FOLIO(folio_test_writeback(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);
	
	nr = folio_nr_pages(folio);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			entry = xas_load(&xas);
			if (!entry || !folio_test_stalesaved(folio)){
				pr_err("enable_swp_entry_remap fail [%p] entry[%lx]stalesaved[%d]", 
							folio, from_entry.val ,folio_test_stalesaved(folio));
				goto unlock;
			}
			to_entry.val = xa_to_value(entry);
			if (to_entry.val) {
				if (unlikely(non_swap_entry(to_entry))){
					pr_err("enable_swp_entry_remap fail [%p] entry[%lx]->entry[%lx]", 
								folio, from_entry.val, to_entry.val);
					goto unlock;
				}
				to_entry_enabled.val = to_entry.val;
				swp_entry_clear_ext(&to_entry_enabled, 0x1); //we cannot use 0x3 to test if locked
				if (swp_entry_test_locked(&to_entry_enabled)){ // 0x2 locket this should be returned
					// pr_err("enable_swp_entry_remap fail locked [%p] $[%d] entry[%lx]->migentry[%lx]", 
					// 		folio, folio_test_swapcache(folio), 
					// 		from_entry.val, to_entry.val);
					locked = true; 
					//message has been passed to this place
					//we should unlock it here, and let do_swap_page do the rest			
					// swp_entry_clear_ext(&to_entry_enabled, 0x2); //we cannot use 0x3 to test if locked
					MULTISWAP_MIG_ERR("enable clear unready, should be deleted by intercepter [%p]ref[%d]$[%d] entry[%lx]->entry[%lx] ", 
						folio, folio_ref_count(folio), folio_test_swapcache(folio), 
						from_entry.val, to_entry_enabled.val);
					xas_store(&xas, xa_mk_value(to_entry_enabled.val + i));
					// xas_store(&xas, xa_mk_value(to_entry.val + i)); //don't touch it
					goto unlock;
				}
				else{
					xas_store(&xas, xa_mk_value(to_entry_enabled.val + i));
					MULTISWAP_MIG_INFO("enable_remap succ, folio[%p]ref[%d] $[%d] entry[%lx]->entry[%lx] ", 
						folio, folio_ref_count(folio), folio_test_swapcache(folio), 
						from_entry.val, to_entry_enabled.val);
					if (unlikely(!folio_test_uptodate(folio)))
						BUG();		
				}
			}
			else{
				pr_err("enable_swp_entry_remap fail [%p] entry[%lx]", folio, from_entry.val);
				goto unlock;
			}
			xas_next(&xas);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN)));

	if (locked){
		swp_entry_clear_ext(&to_entry_enabled, 0x3); //we cannot use 0x3 to test if locked
		p_to_entry->val = to_entry_enabled.val;
		return 1;
	}
	else{
		p_to_entry->val = to_entry_enabled.val;
	}
	if (!xas_error(&xas))
		return 0;

	return xas_error(&xas);
}

/*only supports single page remap*/
int add_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t to_entry, 
			gfp_t gfp)
{
	struct address_space *address_space = swap_address_space_remap(from_entry);
	long nr = folio_nr_pages(folio);
	int i;
	void* old;
	pgoff_t idx = swp_offset(from_entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));

	// xas_set_update(&xas, workingset_update_node);
	VM_WARN_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_FOLIO(!folio_test_swapbacked(folio), folio);
	VM_WARN_ON_FOLIO(!folio_test_stalesaved(folio), folio);

	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			old = xas_load(&xas);
			if (xa_is_value(old)) {
				xas_unlock_irq(&xas);
				return -EEXIST;
			}
			if (!folio_test_stalesaved(folio)){
				xas_unlock_irq(&xas);
				pr_err("add_swp_entry_remap abandon folio[%p] st[%d] ref[%d]", folio, folio_test_stalesaved(folio), folio_ref_count(folio));
				return -EEXIST;
			}
			VM_WARN_ON_FOLIO(xas.xa_index != idx + i, folio);
			VM_WARN_ON_FOLIO(from_entry.val + i != page_private(folio_page(folio, i)), folio);
			xas_store(&xas, xa_mk_value(to_entry.val + i));
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	return xas_error(&xas);
}

static int add_to_swap_cache_save_check(struct folio *folio, swp_entry_t entry,
			gfp_t gfp, bool saved)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	void* _entry;
	unsigned long message;

	if (swp_entry_test_ext(entry)){
		pr_err("add_to_swap_cache_save can't deal ext entry[%lx]", entry.val);
		BUG();
		return -1;
	}
	if (entry_is_entry_ext(folio)){
		pr_err("a buggy folio[%p] entry[%lx]", folio, entry.val);
		BUG();
	}
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			_entry = xas_load(&xas);
			if (_entry){ //normal
				if (xa_is_value(_entry)){
					message = xa_to_value(_entry);
					pr_err("got message[%lx]", message);
					BUG();
					xas_unlock_irq(&xas);
					return -2;
				} 
				else if (entry_is_entry_ext(_entry) == 1){
					if (saved){
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
						//pr_info("got shadow entry [%p], transfer to folio[%p] ", _entry, folio);
#endif
						folio_add_shadow_entry(folio, _entry);
					}
					else{
						pr_err("got shadow entry [%lx], in mig[%lx]", (unsigned long)_entry, entry.val);
						shadow_entry_free(_entry);
					}
				}
				else if (entry_is_entry_ext(_entry) == 0){
					pr_err("add_to_swap_cache_save_check [%lx]->[%p] got folio value ?",
								entry,  _entry);
					xas_unlock_irq(&xas);
					return -3;
				}
				else{
					pr_err("add_to_swap_cache_save_check [%lx]->[%p] got freed entry_ext",
								entry.val,  _entry);
				}
			}

			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			// set_page_private_debug(folio_page(folio, i), entry.val + i, 2);
			xas_store(&xas, folio);
			if (entry_is_entry_ext((void*)folio))
			{
				pr_err("folio misunderstand err[%p]", folio);
				BUG();	
			}
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	pr_err("add_to_swap_cache_save");
	folio_ref_sub(folio, nr);
	BUG();

	return xas_error(&xas);
}
/*
 * add_to_swap_cache resembles filemap_add_folio on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
static int add_to_swap_cache_save(struct folio *folio, swp_entry_t entry,
			gfp_t gfp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	void* _entry;
	if (swp_entry_test_ext(entry)){
		pr_err("add_to_swap_cache_save can't deal ext entry[%lx]", entry.val);
		BUG();
		return -1;
	}
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, folio_order(folio));
	unsigned long i, nr = folio_nr_pages(folio);

	VM_WARN_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr);
	folio_set_swapcache(folio);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_FOLIO(xas.xa_index != idx + i, folio);
			// set_page_private_debug(folio_page(folio, i), entry.val + i, 2);
			_entry = xas_store(&xas, folio);
			if (_entry){
				pr_err("add_to_swap_cache_save got entry?[%lx]", (unsigned long)_entry);
				BUG();
			}
			xas_next(&xas);
		}
		address_space->nrpages += nr;
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	folio_clear_swapcache(folio);
	pr_err("add_to_swap_cache_save");
	BUG();
	folio_ref_sub(folio, nr);
	return xas_error(&xas);
}


swp_entry_t entry_get_migentry_lock(swp_entry_t ori_swap)
{
	swp_entry_t mig_swap, locked_mig;
	struct address_space *address_space_remap;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	long nr = 1, i;
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		if (xa_is_value(entry)){
			mig_swap.val = xa_to_value(entry);
			if (mig_swap.val && non_swap_entry(mig_swap)){
				pr_err("remap err [%lx]->[%lx]", ori_swap.val, mig_swap.val);
				BUG();
			}
			locked_mig.val = mig_swap.val;
			swp_entry_set_locked(&locked_mig);
			xas_store(&xas, xa_mk_value(locked_mig.val));
		}
		else if (entry){
			pr_err("remap inner bug [%lx]->[%p]", ori_swap.val, entry);
			BUG();
		}
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return mig_swap;
}

/* 
 * MULTISWAP:
 * Find mig_entry of original swap entry
 * This function will LOCK the entry->mig_entry[LOCK],
 * for multi-thread protection. 
 */
swp_entry_t entry_get_migentry_unlock(swp_entry_t ori_swap, swp_entry_t _mig_swap)
{
	swp_entry_t mig_swap, locked_mig;
	struct address_space *address_space_remap;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	long i;
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < 1; i++) {
		void *entry = xas_load(&xas);
		if (likely(xa_is_value(entry))){
			mig_swap.val = xa_to_value(entry);
			if (mig_swap.val && non_swap_entry(mig_swap) 
				|| !swp_entry_physical_same(mig_swap, _mig_swap)
				|| !(swp_entry_test_locked(&mig_swap)))
			{
				pr_err("remap err [%lx]->[%lx] <> [%lx]", ori_swap.val, mig_swap.val, _mig_swap.val);
				BUG();
			}
			locked_mig.val = mig_swap.val;
			swp_entry_clear_ext(&locked_mig,  0x2);
			xas_store(&xas, xa_mk_value(locked_mig.val));
		} else {
			pr_err("remap err [%lx]->[%lx] <> [%lx]", ori_swap.val, mig_swap.val, _mig_swap.val);
			BUG();
		}
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return locked_mig;
}

swp_entry_t entry_get_migentry(swp_entry_t ori_swap)
{
	swp_entry_t mig_swap;
	struct address_space *address_space_remap;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	long nr = 1, i;
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		if (xa_is_value(entry))
			mig_swap.val = xa_to_value(entry);
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return mig_swap;
}

static bool entry_remap_empty(swp_entry_t entry)
{
	swp_entry_t mig_swap;
	void* xavalue;
	bool ret;
	pgoff_t idx;
	struct address_space *address_space_remap;
	ret = false;
	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(entry);
	if (!address_space_remap) 
		return false;
	idx = swp_offset(entry);
	XA_STATE(xas, &address_space_remap->i_pages, idx);
	
	xa_lock_irq(&address_space_remap->i_pages);
	xavalue = xas_load(&xas);
	if (xa_is_value(xavalue))
		ret = false;
	else
		ret = true;
	xa_unlock_irq(&address_space_remap->i_pages);

	return ret;
}

/*
 * check if this entry already has some remaps
 * find a sutable version for it if manage to find one, 
 * return version number
 * else, return -1 indicates cannot hold a remap anymore,
 * unless some one fall back and returns a version back
 */
int entry_remap_usable_version(swp_entry_t entry)
{
	swp_entry_t check_entry;
	int i;
	struct address_space *address_space_remap;
	pgoff_t idx;
	void* xavalue;

	address_space_remap = swap_address_space_remap(entry);

	VM_BUG_ON(swp_entry_test_ext(entry));
	i = 0;
	xa_lock_irq(&address_space_remap->i_pages); //lock
	while (i < SWP_ENTRY_ALIVE_VERSION){
		check_entry.val = entry.val;
		swp_entry_set_special(&check_entry, i);
		swp_entry_clear_ext(&check_entry, 0x3); //clear all ext
		idx = swp_offset(check_entry);
		XA_STATE(xas, &address_space_remap->i_pages, idx);

		xavalue = xas_load(&xas);
		if (!xa_is_value(xavalue)) //not occupied
		{
			xa_unlock_irq(&address_space_remap->i_pages);//unlock
			return i;
		}
check_next_version:
		i++;
	}
	xa_unlock_irq(&address_space_remap->i_pages);//unlock

	pr_err("entry_remap_usable_version all occupied [0x%lx]", entry.val);
	return -1;//didn't found

// 	while (i <= SWP_ENTRY_MAX_SPEC){
// 		check_entry.val = entry.val;
// 		swp_entry_set_special(&check_entry, i);
// 		swp_entry_clear_ext(&check_entry, 0x3); //clear all ext
// 		if (entry_remap_empty(check_entry)){
// 			if (i)
// 				pr_err("entry_remap_usable_version first [0x%lx]v[%d]", check_entry.val, i);
// 			return i;
// 		} else {
// 			// pr_err("entry_remap_usable_version $ed [0x%lx]v[%d]", check_entry.val, i);
// 		}
// check_next_version:
// 		i++;
// 	}
}
swp_entry_t folio_get_migentry(struct folio* folio, swp_entry_t ori_swap)
{
	swp_entry_t mig_swap;
	int i;
	long nr;
	struct address_space *address_space_remap;
	VM_WARN_ON_FOLIO((folio_swap_entry(folio).val == 0), folio);

	mig_swap.val = 0;
	address_space_remap = swap_address_space_remap(ori_swap);
	if (!address_space_remap) 
		return mig_swap;
	nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	// xas_set_update(&xas, workingset_update_node);
	VM_WARN_ON_FOLIO((nr > 1), folio);
	VM_WARN_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_FOLIO(folio_test_writeback(folio), folio);
	VM_WARN_ON_FOLIO(!folio_test_locked(folio), folio);
	
	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		VM_BUG_ON_FOLIO(!entry, folio);
		mig_swap.val = xa_to_value(entry);
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);

	return mig_swap;
}
/*
 * This must be called only on folios that have
 * been verified to be in the swap cache.
 */
void __delete_from_swap_cache(struct folio *folio,
			swp_entry_t entry, void *shadow)
{
	struct address_space *address_space = swap_address_space(entry);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(entry);
	XA_STATE(xas, &address_space->i_pages, idx);

	xas_set_update(&xas, workingset_update_node);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);
	MULTISWAP_MIG_INFO_ON(unlikely(folio_test_stalesaved(folio)), 
		"folio[%p] entry[%lx] cnt:%d origin swapcache clearing", 
		folio, entry.val, __swp_swapcount(entry));

	if (unlikely(shadow && !xa_is_value(shadow) && entry_is_entry_ext(shadow)!=1)){
		pr_err("bad shadow $ folio[%p] entry[%lx] shadow[%lx]", folio, entry.val, (unsigned long)shadow);
		shadow = NULL;
		BUG();
	}
	for (i = 0; i < nr; i++) {
		void *entry_ = xas_store(&xas, shadow);
		// if (shadow)
		// 	pr_info("[TRANSFER]__delete_s$ folio[%p]i[%d]->entry[%lx] shadow[%lx] ", folio, i, entry, (unsigned long)shadow);
		// else
		// 	pr_info("[NO EXT]__delete_s$ folio[%p]i[%d]->entry[%lx] shadow[NULL]", folio, i,  entry);
		VM_BUG_ON_PAGE(entry_ != folio, entry_);

		if (unlikely(entry_ != folio)) {
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
			pr_err("__delete_sc mismatch entry[%lx]cnt[%d]->[%p]<>folio[%p] stale[%d]", 
					entry.val, __swap_count(entry), entry_, folio, folio_test_stalesaved(folio));
#endif
			if (entry_)
				BUG();
			xas_next(&xas);
			continue;
		}
		if (unlikely(page_private(folio_page(folio, i)) != entry.val + i)){
			pr_err("try delete from s$2 folio[%p] [%lx]<>[%lx]", folio, page_private(folio_page(folio, i)), entry.val + i);
			BUG();
		}
		// set_page_private_debug(folio_page(folio, i), 0, __FILE__, __LINE__);
		set_page_private(folio_page(folio, i), 0);
		// if (unlikely(swp_entry_test_special(entry) > 0))
		// 	pr_info("__delete_from_swap_cache folio[%p]stale[%d]->shadow[%p] entry[%lx]->ext[%p]", 
		// 		folio, folio_test_stalesaved(folio), folio->shadow_ext, 
		// 		entry.val, shadow);
		xas_next(&xas);
	}
	folio_clear_swapcache(folio);
	// if (folio_test_swappriolow(folio)){
	// 	pr_err("[ckpt2] delete $ entry[%lx]->folio[%p]pri[%lx]", entry.val, folio, page_private(folio_page(folio, 0)));
	// 	dump_stack();
	// }
	if(unlikely((page_private(folio_page(folio, 0)) != 0))){
		pr_err("delete $ entry[%lx]->folio[%p]pri[%lx]", entry.val, folio, page_private(folio_page(folio, 0)));
		BUG();	
	}

	// if (shadow && swp_entry_test_special(entry) > 0)
	// 	pr_info("delete $ entry[%lx]->folio[%p]=>shadow[%p]", entry.val, folio, shadow);

	address_space->nrpages -= nr;
	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

/*make sure entry has no ext set*/
void __delete_from_swap_cache_mig(struct folio *folio,
			swp_entry_t entry, bool shadow_transfer)
{
	struct address_space *address_space = swap_address_space(entry);
	int i;
	pgoff_t idx;
	long nr = folio_nr_pages(folio);
	void *shadow = NULL;

	idx = swp_offset(entry);	
	XA_STATE(xas, &address_space->i_pages, idx);
	xas_set_update(&xas, workingset_update_node);

	// VM_BUG_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	// VM_WARN_ON_FOLIO(!folio_test_swapcache(folio), folio);
	// VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry_ = xas_load(&xas);
		if (entry_ != folio) {
			pr_err("__delete_sc_mig mismatch entry[%lx]->folio[%p] pri[%lx]", 
					swp_offset(entry), folio, folio_swap_entry(folio).val);
			BUG();
		}
		// if (page_private(folio_page(folio, i)) != 0){
		// 	pr_err("try delete from s$ folio[%p] pri[%lx]<>[0]", 
		// 					folio, page_private(folio_page(folio, i)));
			// set_page_private_debug(folio_page(folio, i), 0, 4);
		// }
		if (shadow_transfer){
			shadow = folio_remove_shadow_entry(folio);
			if (shadow){ //may transfer, check
				if (entry_is_entry_ext(shadow) == 1){
					xas_store(&xas, shadow);
					xas_next(&xas);
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
					pr_info("[TRANSFER]__delete_sc_mig folio[%p]->entry[%lx] ext[%p]", 
								folio, entry.val, shadow);
#endif
					continue;
				}
				else{
					pr_err("__delete_sc_mig fail transfer folio[%p]->shadow[%p]", folio, shadow);
					BUG();
				}
			}
		}
		//no transfer, store a NULL
		xas_store(&xas, NULL);
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
// #ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
// 	pr_info("delete mig $ entry[%lx]->folio[%p] shadow[%p] ref[%d]", 
// 				entry.val, folio, shadow, folio_ref_count(folio));
// #endif
	__node_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	__lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr);
}

void __delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to, bool delete_unpepared)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(entry_from);
	XA_STATE(xas, &address_space->i_pages, idx);
	swp_entry_t tmp;

	// VM_WARN_ON_FOLIO(!folio_test_stalesaved(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	//VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	// VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio); //removed for cases in invalid success swap_free

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL); //clear
		tmp.val = xa_to_value(entry);
		swp_entry_clear_ext(&tmp, 0x3);
		if (delete_unpepared){
			swp_entry_clear_ext(&entry_to, 0x3);
		}
		if (entry_to.val && tmp.val != entry_to.val){
			pr_err("[%lx]->[%lx]<>[%lx] mismatch",   //means we don't care about it
					entry_from.val, entry_to.val, tmp.val);
			dump_stack();		
		} //entry_to.val == 0 
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
	MULTISWAP_MIG_INFO("delete_from_swap_remap folio[%p] [%lx]->[%lx]", 
		folio, entry_from.val, entry_to.val);
}
void delete_from_swap_remap_raw(swp_entry_t entry_from, swp_entry_t entry_to)
{
	struct address_space *address_space_remap = swap_address_space_remap(entry_from);
	int i;
	long nr = 1;
	pgoff_t idx = swp_offset(entry_from);
	XA_STATE(xas, &address_space_remap->i_pages, idx);
	swp_entry_t tmp;
	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL); //clear
		tmp.val = xa_to_value(entry);
		swp_entry_clear_ext(&tmp, 0x3);

		//deal with unprepared
		swp_entry_clear_ext(&entry_to, 0x3);
		if (entry_to.val && tmp.val != entry_to.val){
			pr_err("delete_from_swap_remap_raw [%lx]->[%lx]<>[%lx] mismatch",   //means we don't care about it
					entry_from.val, entry_to.val, tmp.val);
			BUG();		
		} //entry_to.val == 0 
		xas_next(&xas);
	}
	address_space_remap->nrpages -= nr;
	xa_unlock_irq(&address_space_remap->i_pages);
	MULTISWAP_MIG_INFO("delete_from_swap_remap_raw [%lx]->[%lx]", 
		entry_from.val, entry_to.val);
}
static void __delete_from_swap_remap_get_mig(struct folio *folio, swp_entry_t entry_from, swp_entry_t* entry_to)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	int i;
	long nr = folio_nr_pages(folio);
	pgoff_t idx = swp_offset(entry_from);
	XA_STATE(xas, &address_space->i_pages, idx);
	swp_entry_t tmp;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	// VM_BUG_ON_FOLIO(folio_test_writeback(folio), folio);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL); //clear
		tmp.val = xa_to_value(entry);
		if (non_swap_entry(tmp)){ //entry_to.val == 0 
			pr_err("2[%lx]-<>[%lx] mismatch",   //means we don't care about it
					entry_from.val,  tmp.val);
			BUG();			
		}
		entry_to->val = tmp.val;
		xas_next(&xas);
	}
	address_space->nrpages -= nr;
	MULTISWAP_MIG_INFO("__delete_from_swap_remap_get_mig folio[%p] [%lx]->[%lx] wb[%d]", 
		folio, entry_from.val, entry_to->val, folio_test_writeback(folio));
}
/**
 * add_to_swap - allocate swap space for a folio
 * @folio: folio we want to move to swap
 *
 * Allocate swap space for the folio and add the folio to the
 * swap cache.
 *
 * Context: Caller needs to hold the folio lock.
 * Return: Whether the folio was added to the swap cache.
 */
bool add_to_swap(struct folio *folio, long* left_space)
{
	swp_entry_t entry;
	int err;
	void* shadow_test = NULL;
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_uptodate(folio), folio);

	entry = folio_alloc_swap(folio, left_space, false, true);
			//folio_alloc_swap(folio, left_space, false);
	if (!entry.val)
		return false;
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR
	if (swp_entry_test_special(entry) > 0){
		count_memcg_folio_events(folio, SWAP_STALE_SAVE_REUSE, folio_nr_pages(folio));
		// if (swp_entry_test_special(entry) > 0)
		// 	pr_info("folio_alloc_swap normal entry[%lx] v[%d] for folio[%p] cnt:%d",
		// 		 entry.val, swp_entry_test_special(entry), folio, __swap_count(entry));		
	}
#endif
	
	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	err = add_to_swap_cache(folio, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, &shadow_test);
	if (err){
		if (shadow_test){
			pr_err("add to swap should fail get shadow folio[%p] entry[%lx]cnt[%d] shadow[%p]",
					folio, entry.val, __swap_count(entry), shadow_test);
			shadow_entry_free(shadow_test);
		}
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	}


	// pr_err("ckpt2 folio[%p]<-entry[%lx]", folio, entry.val);
	/*
	 * Normally the folio will be dirtied in unmap because its
	 * pte should be dirty. A special case is MADV_FREE page. The
	 * page's pte could have dirty bit cleared but the folio's
	 * SwapBacked flag is still set because clearing the dirty bit
	 * and SwapBacked flag has no lock protected. For such folio,
	 * unmap will not set dirty bit for it, so folio reclaim will
	 * not write the folio out. This can cause data corruption when
	 * the folio is swapped in later. Always setting the dirty flag
	 * for the folio solves the problem.
	 */
	folio_mark_dirty(folio);

	return true;

fail:
	pr_err("add_to_swap failling folio[%p], entry[%lx]", folio, entry.val);
	put_swap_folio(folio, entry);
	return false;
}

/*
 * the caller has a reference on the folio.
 */
void delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to, bool delete_unpepared){
	struct address_space *address_space = swap_address_space_remap(entry_from);
	if (unlikely(!address_space)){
		pr_err("delete_from_swap_remap address space BUG");
		return;
	}
	if (!folio){
		pr_err("delete_from_swap_remap address no folio");
		return;
	}
	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_remap(folio, entry_from, entry_to, delete_unpepared);
	xa_unlock_irq(&address_space->i_pages);
}
static void __clear_swap_remap_range(swp_entry_t entry_start, int order)
{
	swp_entry_t tmp;
	long i;
	struct address_space *address_space = swap_address_space_remap(entry_start);
	pgoff_t idx = swp_offset(entry_start);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, order);
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < (1 << order); i++) {
			void *entry = xas_store(&xas, NULL); //clear
			tmp.val = xa_to_value(entry);
			
			if (tmp.val && !non_swap_entry(tmp)){ //entry_to.val != 0
				if (swp_entry_test_ext(tmp) & 0x3){
					pr_err("__clear_swap_remap_range[%lx]->[%lx] locked/inprocess? store back", 
							swp_entry(swp_type(entry_start), idx++).val,  tmp.val);	
					xas_store(&xas, xa_mk_value(tmp.val));	
				}
				else{
					pr_info("__clear_swap_remap_range[%lx]->[%lx]", 
							swp_entry(swp_type(entry_start), idx++).val,  tmp.val);					
					swap_free_mig(tmp);
				}
			}
			address_space->nrpages -= 1;
			xas_next(&xas);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, GFP_KERNEL));
	if (!xas_error(&xas))
		return;
	BUG();
}
static void clear_swap_remap_range(struct swap_info_struct *si, int start, int end)
{
	swp_entry_t entry_start = swp_entry(si->type, start);
	struct address_space *address_space = swap_address_space_remap(entry_start);
	__clear_swap_remap_range(entry_start, SWAP_ADDRESS_SPACE_REMAP_SHIFT);
	pr_info("clear_swap_remap_range type[%d][%d-%d] left[%ld]", 
			si->type, start, end, address_space->nrpages);
}

void clear_swap_remap_entire(struct swap_info_struct *si)
{
	int maxpages = si->max;
	int start = 0;
	for(; start < maxpages; start += SWAP_ADDRESS_SPACE_REMAP_PAGES)
	{
		clear_swap_remap_range(si, start, start + SWAP_ADDRESS_SPACE_REMAP_PAGES);
	}
}

void swap_remap_unlock(struct folio *folio, swp_entry_t ori_swap, swp_entry_t mig_swap){
	swp_entry_t _locked_mig_swap, locked_mig;
	struct address_space *address_space_remap = swap_address_space_remap(ori_swap);
	long nr = 1, i;
	pgoff_t idx = swp_offset(ori_swap);
	XA_STATE(xas, &address_space_remap->i_pages, idx);

	xa_lock_irq(&address_space_remap->i_pages);
	for (i = 0; i < nr; i++) {
		void *entry = xas_load(&xas);
		if (xa_is_value(entry)){
			_locked_mig_swap.val = xa_to_value(entry);
			swp_entry_clear_ext(&_locked_mig_swap, 0x3);
			if (unlikely(_locked_mig_swap.val && non_swap_entry(_locked_mig_swap))){
				pr_err("remap err [%lx]->[%lx]", ori_swap.val, mig_swap.val);
				BUG();
			}
			if (_locked_mig_swap.val != mig_swap.val){
				pr_err("remap mismatch [%lx]->[%lx]", _locked_mig_swap.val, mig_swap.val);
				BUG();
			}
			xas_store(&xas, xa_mk_value(mig_swap.val));
			pr_err("remap unlock[%lx]->[%lx]", ori_swap.val, mig_swap.val);
		}
		else if (unlikely(entry)){
			pr_err("remap inner bug [%lx]->[%p]",  ori_swap.val, entry);
			BUG();
		}
		xas_next(&xas);
	}
	xa_unlock_irq(&address_space_remap->i_pages);
}

void delete_from_swap_remap_get_mig(struct folio* folio, swp_entry_t entry_from, swp_entry_t* entry_to)
{
	struct address_space *address_space = swap_address_space_remap(entry_from);
	if (unlikely(!address_space)){
		pr_err("delete_from_swap_remap address space BUG");
		return;
	}
	if (unlikely(!folio)){
		pr_err("delete_from_swap_remap address no folio");
		return;
	}
	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_remap_get_mig(folio, entry_from, entry_to);
	xa_unlock_irq(&address_space->i_pages);
	swp_entry_clear_ext(entry_to, 0x3); 
	// folio_ref_sub(folio, folio_nr_pages(folio));
	MULTISWAP_MIG_INFO("delete_from_swap_remap_get_mig folio[%p]ref[%d] [%lx]->[%lx]", 
		folio, folio_ref_count(folio), entry_from.val, entry_to->val);
}
/*
 * This must be called only on folios that have
 * been verified to be in the swap cache and locked.
 * It will never put the folio into the free list,
 * the caller has a reference on the folio.
 */
swp_entry_t delete_from_swap_cache(struct folio *folio)
{
	swp_entry_t entry = folio_swap_entry(folio);
	struct swap_info_struct *si;
	struct address_space *address_space = swap_address_space(entry);
	void *shadow = NULL;

	// pr_err("DELETE_FROM_SWAP_CACHE: entry[%lx] folio[%p] shadow_ext[%p] mapped[%d]",
	// 	   entry.val, folio, folio->shadow_ext, folio_mapped(folio));

	if (folio_test_swappriohigh(folio) || folio_test_swappriolow(folio)){
		pr_err("delete_s$ folio[%p]->ext[%p] pri[%lx], WARN", folio, folio->shadow_ext, entry.val);
		// BUG();
	}

#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	/* Keep shadow_ext on folio for next eviction, clear xarray slot */
	/* Swap entry is being freed, page stays in memory with its shadow_ext */
	// if (folio->shadow_ext && entry_is_entry_ext(folio->shadow_ext) == 1) {
	// 	pr_info("delete_from_swap_cache: keeping shadow_ext[%p] on folio[%p] for entry[%lx]",
	// 			folio->shadow_ext, folio, entry.val);
	// } else {
	// 	pr_info("delete_from_swap_cache: NO shadow_ext for entry[%lx], folio->shadow_ext=%p",
	// 			entry.val, folio->shadow_ext);
	// }
	/* shadow stays NULL - xarray slot cleared, folio keeps shadow_ext for re-eviction */
#endif

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(folio, entry, shadow);
	xa_unlock_irq(&address_space->i_pages);

	si = get_swap_device(entry);
	if (si->prio >= 100){ // -2 1005
		count_memcg_folio_events(folio ,SWAP_FREE_FAST, folio_nr_pages(folio));
	}else{
		count_memcg_folio_events(folio ,SWAP_FREE_SLOW, folio_nr_pages(folio));
	}
	put_swap_device(si);

	put_swap_folio(folio, entry);
	// if (swp_entry_test_special(entry) > 0){
	// 	pr_info("delete_s$ folio[%p]->ext[%p]", folio, folio->shadow_ext);
	// }
	// if (swp_entry_test_special(entry))
	// 	pr_err("after delete_s$ folio[%p]->ext[%p]", folio, folio->shadow_ext);
	folio_ref_sub(folio, folio_nr_pages(folio));
	if (folio_test_swappriohigh(folio)){
		pr_err("delete_from_swap_cache folio[%p] ref[%d] subbed", folio, folio_ref_count(folio));
		dump_stack();
	}
	return entry;
}

swp_entry_t delete_from_swap_cache_debug(struct folio *folio, swp_entry_t expected_entry)
{
	swp_entry_t entry = folio_swap_entry(folio);
	struct swap_info_struct *si;
	struct address_space *address_space = swap_address_space(entry);
	void *shadow = NULL;

	VM_BUG_ON(entry.val != expected_entry.val);

	// pr_err("DELETE_FROM_SWAP_CACHE_DEBUG: entry[%lx] folio[%p] shadow_ext[%p] mapped[%d]",
	// 	   entry.val, folio, folio->shadow_ext, folio_mapped(folio));

	if (folio_test_swappriohigh(folio) || folio_test_swappriolow(folio)){
		pr_err("delete_s$_debug folio[%p]->ext[%p] pri[%lx], WARN", folio, folio->shadow_ext, entry.val);
		// BUG();
	}

#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	/* Keep shadow_ext on folio for next eviction, clear xarray slot */
	/* Swap entry is being freed, page stays in memory with its shadow_ext */
	// if (folio->shadow_ext && entry_is_entry_ext(folio->shadow_ext) == 1) {
	// 	pr_info("delete_from_swap_cache_debug: keeping shadow_ext[%p] on folio[%p] for entry[%lx]",
	// 			folio->shadow_ext, folio, entry.val);
	// } else {
	// 	pr_info("delete_from_swap_cache_debug: NO shadow_ext for entry[%lx], folio->shadow_ext=%p",
	// 			entry.val, folio->shadow_ext);
	// }
	/* shadow stays NULL - xarray slot cleared, folio keeps shadow_ext for re-eviction */
#endif

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(folio, entry, shadow);
	xa_unlock_irq(&address_space->i_pages);

	si = get_swap_device(entry);
	if (si->prio >= 100){ // -2 1005
		count_memcg_folio_events(folio ,SWAP_FREE_FAST, folio_nr_pages(folio));
	}else{
		count_memcg_folio_events(folio ,SWAP_FREE_SLOW, folio_nr_pages(folio));
	}
	put_swap_device(si);

	put_swap_folio(folio, entry);
	// if (swp_entry_test_special(entry) > 0){
	// 	pr_info("delete_s$ folio[%p]->ext[%p]", folio, folio->shadow_ext);
	// }
	// if (swp_entry_test_special(entry))
	// 	pr_err("after delete_s$ folio[%p]->ext[%p]", folio, folio->shadow_ext);
	folio_ref_sub(folio, folio_nr_pages(folio));
	if (folio_test_swappriohigh(folio)){
		pr_err("delete_from_swap_cache folio[%p] ref[%d] subbed", folio, folio_ref_count(folio));
		dump_stack();
	}
	return entry;
}

/*needs put_swap_folio after it*/
void delete_from_swap_cache_mig(struct folio* folio, swp_entry_t entry, bool dec_count, bool shadow_transfer)
{
	swp_entry_t pgentry = folio_swap_entry(folio), entry_;
	struct address_space *address_space = swap_address_space(entry);
	entry_.val = entry.val;
	swp_entry_clear_ext(&entry_, 0x3); 
		//mig cache only maps mig_entry to folio
		//it is not responsible for validation
	// VM_BUG_ON_FOLIO(!(entry.val == pgentry.val), folio);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache_mig(folio, entry_, shadow_transfer);
	xa_unlock_irq(&address_space->i_pages);

	if (dec_count)
		put_swap_folio(folio, entry_);
	if (!dec_count && __swap_count(entry_) != 1){
		pr_err("folio[%p] ref[%d]entry get out of cache mig[%lx]cnt[%d] lock[%d]", 
				folio, folio_ref_count(folio), entry_.val, __swap_count(entry_), folio_test_locked(folio));	
		// BUG();	
	}
	folio_ref_sub(folio, folio_nr_pages(folio));
	// if (folio_test_swappriohigh(folio)){
	// 	pr_err("delete_from_swap_cache_mig folio[%p] pri[%lx]ref[%d] a[%d]d[%d]st[%d]$[%d] subbed", 
	// 			folio, folio_swap_entry(folio).val, folio_ref_count(folio), folio_test_active(folio), 
	// 			folio_test_dirty(folio), folio_test_stalesaved(folio), folio_test_swapcache(folio));
	// 	// dump_stack();
	// }
}


void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end, int free)
{
	unsigned long curr = begin;
	void *old, *_entry;
	for (;;) {
		swp_entry_t entry = swp_entry(type, curr);
		struct address_space *address_space = swap_address_space(entry);
		XA_STATE(xas, &address_space->i_pages, curr);

		xas_set_update(&xas, workingset_update_node);

		xa_lock_irq(&address_space->i_pages);
		xas_for_each(&xas, old, end) {
			if (!xa_is_value(old) && entry_is_entry_ext(old) < 1)
				continue;
			if (free && old && entry_is_entry_ext(old) == 1){
				// pr_info("clear_shadow_from_s shadow[%p]", old);
				_entry = xas_store(&xas, NULL);
				if (unlikely(old != _entry))
					BUG();
				/* Note: This is NOT timeout-based release, it's normal swap entry cleanup */
				/* Timeout-based counting is done in workingset shadow shrinker */
				shadow_entry_free(old);
				// trace_shadow_entry_free(old, 6);
				continue;
			}
			else if (entry_is_entry_ext(old) == 1){
				pr_err("entry[%lx]clear_shadow_from_s [%p]status[%d] lost control", 
						entry.val, old, entry_is_entry_ext(old));
			}
		}
		xa_unlock_irq(&address_space->i_pages);

		/* search the next swapcache until we meet end */
		curr >>= SWAP_ADDRESS_SPACE_SHIFT;
		curr++;
		curr <<= SWAP_ADDRESS_SPACE_SHIFT;
		if (curr > end)
			break;
	}
}

/* 
 * If we are the only user, then try to free up the swap cache. 
 * 
 * Its ok to check the swapcache flag without the folio lock
 * here because we are going to recheck again inside
 * folio_free_swap() _with_ the lock.
 * 					- Marcelo
 */
void free_swap_cache(struct page *page)
{
	struct folio *folio = page_folio(page);

	if (folio_test_swapcache(folio) && !folio_mapped(folio) &&
	    folio_trylock(folio)) {
		if(unlikely(folio_test_swappriohigh(folio) || folio_test_swappriolow(folio))){
			// folio_clear_swappriohigh(folio);
			// folio_clear_swappriolow(folio);
			pr_info("free_swap_cache folio[%p]wb[%d]$[%d] pri[%lx] force frees_swap pass", 
					folio, folio_test_writeback(folio), folio_test_swapcache(folio), folio_swap_entry(folio).val);
		}
		folio_free_swap(folio);
		folio_unlock(folio);
	}
}

/* 
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	free_swap_cache(page);
	if (!is_huge_zero_page(page))
		put_page(page);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct encoded_page **pages, int nr)
{
	lru_add_drain();
	for (int i = 0; i < nr; i++)
		free_swap_cache(encoded_page_ptr(pages[i]));
	release_pages(pages, nr);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead);
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/*
 * Lookup a swap entry in the swap cache. A found folio will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the folio
 * lock before returning.
 *
 * Caller must lock the swap device or hold a reference to keep it valid.
 */
struct folio *swap_cache_get_folio(struct swap_info_struct * si, swp_entry_t entry,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct folio *folio;
	unsigned long offset_v;
	unsigned long version;
	bool is_stale_saved_folio;
	offset_v = swp_offset(entry);//swp_raw_offset(entry);
	version = swp_entry_test_special(entry);
	is_stale_saved_folio = false;
	// if (version){
	// 	offset_v = offset_v + si->max * version;
	// }
	if (data_race(si->flags & SWP_SYNCHRONOUS_IO) && !non_swap_entry(entry)){
		folio = syncio_swapcache_get_folio(swap_address_space(entry), offset_v ,&is_stale_saved_folio);
	}
	else{
		folio = filemap_get_folio(swap_address_space(entry), offset_v);
	}
	if (folio) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;
		// if (version)
		// 	pr_err("swap_cache_get_folio offset_v[%lx], ver[%lu] entry[%lx] folio[%p]", 
		// 				offset_v, version, entry.val, folio);
		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(folio_test_large(folio)))
			return folio;

		readahead = folio_test_clear_readahead(folio);
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead){
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
				/*DJL ADD BEGIN*/
				trace_swapin_readahead_hit(folio);		
				/*DJL ADD END*/
			}
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
		}

		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra)
				atomic_inc(&swapin_readahead_hits);
		}
	}

	return folio;
}

/*
 * Lookup a swap entry in the swap cache. A found folio will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the folio
 * lock before returning.
 *
 * Caller must lock the swap device or hold a reference to keep it valid.
 */
static struct folio *raw_swap_cache_get_folio(struct swap_info_struct * si, swp_entry_t entry)
{
	struct folio *folio = NULL;
	unsigned long offset_v;
	bool is_stale_saved_folio;
	offset_v = swp_offset(entry);
	is_stale_saved_folio = false;

	if (data_race(si->flags & SWP_SYNCHRONOUS_IO) && !non_swap_entry(entry)){
		folio = syncio_swapcache_get_folio(swap_address_space(entry), offset_v ,&is_stale_saved_folio);
	}
	else{
		folio = filemap_get_folio(swap_address_space(entry), offset_v);
		if (unlikely(folio)){
			pr_info("scgf async return entry[%lx]->[%p]", entry.val, folio);
		}
	}
	return folio;
}

/**
 * filemap_get_incore_folio - Find and get a folio from the page or swap caches.
 * @mapping: The address_space to search.
 * @index: The page cache index.
 *
 * This differs from filemap_get_folio() in that it will also look for the
 * folio in the swap cache.
 *
 * Return: The found folio or %NULL.
 */
struct folio *filemap_get_incore_folio(struct address_space *mapping,
		pgoff_t index)
{
	swp_entry_t swp;
	struct swap_info_struct *si;
	struct folio *folio = __filemap_get_folio(mapping, index, FGP_ENTRY, 0);

	if (!xa_is_value(folio))
		goto out;
	if (!shmem_mapping(mapping))
		return NULL;
	pr_err("undefined result");
	swp = radix_to_swp_entry(folio);
	/* There might be swapin error entries in shmem mapping. */
	if (non_swap_entry(swp))
		return NULL;
	/* Prevent swapoff from happening to us */
	si = get_swap_device(swp);
	if (!si)
		return NULL;
	index = swp_offset(swp);
	folio = filemap_get_folio(swap_address_space(swp), index);
	put_swap_device(si);
out:
	return folio;
}
static inline int should_try_change_swap_entry(int rf_dist, int swap_level, bool loop){
	if (loop){
		if (swap_level == 1){ //refault from fast
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist > 2) return 1;
#endif
		}else if (swap_level == -1){ //refault from slow
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist < 2) return 1;
#endif
		}
	}
	else{
		if (swap_level == 1){ //refault from fast
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist > 3) return 1;
#endif
		}
		else if (swap_level == -1){	//refault from slow
#ifdef CONFIG_LRU_GEN_FALSE_FAST_ASSIGN_PUNISHMENT
			if (rf_dist < 2) return 1;
#endif
		}
	}
	return 0;
} 
struct page*__read_swap_cache_async_save(swp_entry_t entry, 	
	gfp_t gfp_mask, struct vm_area_struct *vma, 
	unsigned long addr,	bool *new_page_allocated, 
	bool no_ra, int* try_free_entry, bool nocheck)
{
	struct swap_info_struct *si;
	struct folio *folio;
	void *shadow = NULL;
	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after swap_cache_get_folio() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			return NULL;
		folio = filemap_get_folio(swap_address_space(entry),
						swp_offset(entry));
		put_swap_device(si);

		MULTISWAP_MIG_INFO_ON(folio, 
			"found already in swap_cache[%lx]->folio[%p]pri[%lx] ref[%d]stale[%d]update[%d]lru[%d]", 
			entry.val, folio, page_private(folio_page(folio, 0)), folio_ref_count(folio), 
			folio_test_stalesaved(folio), folio_test_uptodate(folio), folio_test_lru(folio));
		if (folio)
			return folio_file_page(folio, swp_offset(entry));

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled){
			pr_err("save swap count entry[%lx][%d] not used", entry.val, __swp_swapcount(entry));
			//we should try to clean it
			return NULL;			
		}


		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		folio = vma_alloc_folio(gfp_mask, 0, vma, addr, false);
		if (!folio){
			pr_err("__read_swap_cache_async_save alloc fail addr[%lx] entry[%lx]", addr, entry.val);
			return NULL;
		}

		// /*
		//  * Swap entry may have been freed since our caller observed it.
		//  */
		err = swapcache_prepare(entry);
		if (!err)
			break;
		MULTISWAP_MIG_ERR("mig swapcache_prepare fail[%d] addr[%lx] entry[%lx]", 
			err, addr, entry.val);
		if (nocheck && !folio_test_locked(folio))
			break;
		folio_put(folio);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}
	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */

	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);
	if (mem_cgroup_swapin_charge_folio(folio, NULL, gfp_mask, entry))
		goto fail_unlock;
	MULTISWAP_MIG_INFO("__read_swap_cache_async_save folio[%p]uptodate[%d] charged by memcg[%d]", 
		folio, folio_test_uptodate(folio), mem_cgroup_id(folio_memcg(folio)));
	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(folio, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow))
		goto fail_unlock;

//	mem_cgroup_swapin_uncharge_swap(entry); //DJL doesn't count this
	//now shadow has been used
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	if(folio)
		ASSERT_FOLIO_NO_SE(folio, __FILE__, __LINE__);
	if (entry_is_entry_ext(shadow) == 1){
		folio_add_shadow_entry(folio, shadow);
		MULTISWAP_MIG_INFO("[TRANSFER]read_save entry[%lx]=>folio[%p] ext[%p]", 
			entry.val, folio, folio->shadow_ext);
	}
#else
	if (entry_is_entry_ext(shadow) > 0){
		pr_err("[FREE]__read_swap_cache_async_save free entry[%p]", shadow);
		shadow_entry_free(shadow);
	}
#endif
	/*DJL ADD END*/

	/* Caller will initiate read into locked folio */
	// folio_add_lru(folio);

	if (new_page_allocated)
		*new_page_allocated = true;

	return &folio->page; 

fail_unlock:
	put_swap_folio(folio, entry);
	folio_unlock(folio);
	folio_put(folio);
	return NULL;
}
/*DJL ADD BEGIN*/
struct page *__read_swap_cache_async(swp_entry_t entry, 	
	gfp_t gfp_mask, struct vm_area_struct *vma, 
	unsigned long addr,	unsigned long real_addr, bool *new_page_allocated, 	bool no_ra, 
	int* try_free_entry, bool allow_null)
/*DJL ADD END*/
{
	struct swap_info_struct *si;
	struct folio *folio;
	void *shadow = NULL;
	bool abandon_shadow = false;
	/*DJL ADD BEGIN*/
	int swap_level = -2;
	int rf_dist_ts = -1;
	/*DJL ADD END*/
	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after swap_cache_get_folio() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si){
			pr_err("entry[%lx] got not si", entry.val);
			return NULL;
		}
		folio = filemap_get_folio(swap_address_space(entry),
						swp_offset(entry));
		put_swap_device(si);
		if (folio)
			return folio_file_page(folio, swp_offset(entry));

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled){
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
			if (!allow_null && !entry_get_migentry(entry).val){
				pr_info("swap count entry[%lx][%d] not used exact[%d]", 
						entry.val, __swp_swapcount(entry), allow_null);
				// BUG();
			}
#endif
			return NULL;
		}

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		folio = vma_alloc_folio(gfp_mask, 0, vma, addr, false);
		if (!folio){
			pr_err("__read_swap_cache_async alloc fail addr[%lx] entry[%lx]", addr, entry.val);
			return NULL;
		}
		ASSERT_FOLIO_NO_SE(folio, __FILE__, __LINE__);
		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
		// pr_info("swapcache_prepare fail[%d][%s] addr[%lx] entry[%lx]",
		// 			err, err == -EINVAL ? "EINVAL" : (err == -EEXIST ? "EEXIST" : "?"), addr, entry.val);
#endif
		folio_put(folio);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */

	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);

	if (mem_cgroup_swapin_charge_folio(folio, NULL, gfp_mask, entry)){
		pr_err("mem_cgroup charge fail");
		goto fail_unlock;
	}
	if (page_private(folio_page(folio, 0))){
		pr_err("before add_to_swap_cache folio[%p] private[%lx]", folio, page_private(folio_page(folio, 0)));
	}
	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(folio, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow)){
		pr_err("add_to_swap_cache fail");
		goto fail_unlock;
	}

	mem_cgroup_swapin_uncharge_swap(entry);
	si = swp_swap_info(entry);
#ifdef CONFIG_LRU_GEN_PASSIVE_SWAP_ALLOC //decomment all below to enable
	// folio_clear_swappriohigh(folio); //clear here
	// folio_set_swappriolow(folio);  //set here so that only those got promoted again can go to fast
	// if (si->prio == get_fastest_swap_prio()){
	// 	folio_swapprio_promote(folio);
	// }
#endif
	if (swap_info_is_fastest(si)){
		swap_level = 1;
	}
	else if (swap_info_is_slowest(si)){
		swap_level = -1;
	}
	else
		swap_level = 0;

	/*DJL ADD BEGIN*/
	if (shadow){
		workingset_refault(folio, shadow, &rf_dist_ts, real_addr, swap_level, entry, &abandon_shadow);
		if (vma && vma->vm_mm){
			if (swap_info_is_fastest(si)){
				count_memcg_event_mm(vma->vm_mm, WORKINGSET_REFAULT_FAST);
			}
			else if (swap_info_is_slowest(si)){
				count_memcg_event_mm(vma->vm_mm, WORKINGSET_REFAULT_SLOW);
			}
			*try_free_entry = should_try_change_swap_entry(
						rf_dist_ts >= MAX_NR_GENS ? rf_dist_ts - 4 : rf_dist_ts, 
						swap_level, rf_dist_ts >= MAX_NR_GENS ? 0 : 1);
		}
	}
	else{
		trace_folio_ws_chg(folio, addr, folio_pgdat(folio), -1, 0, 0, 1, swap_level, -2, (unsigned long)entry.val);
	}

	//now shadow has been used
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
	if (folio->shadow_ext){
		if (folio->shadow_ext != shadow){
			pr_err("[FREE]folio[%p]->shadowext[%p] shadow[%p]", 
			folio, folio->shadow_ext, shadow);
			shadow_entry_free(folio->shadow_ext);
			trace_shadow_entry_free(folio->shadow_ext, 7);	
		}
		BUG();
		folio->shadow_ext = NULL;
	}
	ASSERT_FOLIO_NO_SE(folio, __FILE__, __LINE__);
	abandon_shadow = false;
	if (entry_is_entry_ext(shadow) == 1){
		if (likely(!abandon_shadow)){
			folio_add_shadow_entry(folio, shadow);
		}
		else{
			shadow_entry_free(shadow);
			trace_shadow_entry_free(shadow, 8);	
			pr_info("[ABANDON]read_cache entry[%lx]=>folio[%p] stale free ext[%p]", 
					entry.val, folio, shadow);
		}
	}
#else
	if (entry_is_entry_ext(shadow) > 0){
		pr_err("[FREE]__read_swap_cache_async free entry[%p]", shadow);
		shadow_entry_free(shadow);
	}
#endif
	/*DJL ADD END*/
	/* Caller will initiate read into locked folio */
	// folio_add_lru(folio);
	if (no_ra)
		folio_add_lru(folio);
	else
		folio_add_lru_ra(folio);		

	if (new_page_allocated)
		*new_page_allocated = true;

//reclaim another 4kb pages space //1mb = 128*4kb pages space
#ifdef CONFIG_LRU_GEN_CGROUP_KSWAPD_BOOST
	if (force_wake_up_delay_now++ >= force_wake_up_delay){
		force_wake_up_delay_now = 0;
		swapin_force_wake_kswapd(gfp_mask, 0);
	}
#endif
	return &folio->page;

fail_unlock:
	put_swap_folio(folio, entry);
	folio_unlock(folio);
	folio_put(folio);
	return NULL;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
				   struct vm_area_struct *vma,
				   unsigned long addr, bool do_poll,
				   struct swap_iocb **plug, bool count, int* try_free_entry)  //DJL ADD PARA
{
	bool page_was_allocated;
	/*DJL ADD BEGIN*/
	struct swap_info_struct *si;
	/*DJL ADD END*/
	struct page *retpage = __read_swap_cache_async(entry, gfp_mask,
			vma, addr, 0, &page_was_allocated, true, try_free_entry, false);
	
	if (page_was_allocated)
		swap_readpage(retpage, do_poll, plug);
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
	else if (!retpage){
		// pr_info("read_swap_cache_async alloc fail entry[%lx]", entry.val);
	}
#endif
	/*DJL ADD BEGIN*/
	if (count && page_was_allocated){
		si = get_swap_device(entry);
		if (swap_info_is_fastest(si)){
			count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST);
			// pr_err("read_swap_cache_async entry[%lx]", entry.val);
		} 
		else if (swap_info_is_slowest(si)){
			count_memcg_event_mm(vma->vm_mm, SWAPIN_SLOW);
		} 
		else{
			count_memcg_event_mm(vma->vm_mm, SWAPIN_MID);
		}
		put_swap_device(si);
	}
	/*DJL ADD END*/
	return retpage;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

static unsigned int __swapin_nr_pages_boost(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 4;
	if (pages == 4) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset - prev_offset > (prev_win / 2) ||  prev_offset - offset > (prev_win / 2))
			pages = 1;//force add one page in any cases
	} else {
		unsigned int roundup = 8;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win * 3 / 4;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}
/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * This has been extended to use the NUMA policies from the mm triggering
 * the readahead.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 */
struct page *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf, int* try_free_entry)
{
	struct page *page;
	unsigned long entry_offset = swp_raw_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	bool do_poll = true, page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;
	unsigned long real_addr = vmf->real_address;
	int try_free;

	mask = swapin_nr_pages(offset) - 1;
	if (!mask)
		goto skip;

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset ; offset++) {
		/* Ok, do the async read-ahead now */
		page = __read_swap_cache_async(
			swp_entry(swp_type(entry), offset),
			gfp_mask, vma, addr, real_addr, &page_allocated, true, &try_free, false); //DJL doesn't support fast swapout
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false, &splug);
			if (offset != entry_offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
			}
		}
		put_page(page);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);

	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	/* The page was likely read above, so no need for plugging here */
	return read_swap_cache_async(entry, gfp_mask, vma, addr, do_poll, NULL, false, try_free_entry);
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space, *spaces_remap, *space_remap;
	unsigned int i, nr, nr_remap;
	nr_pages = nr_pages; //do this to support 64 remap
	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	nr = nr << (SWP_SPECIAL_MARK);
	nr_remap = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_REMAP_PAGES);
	nr_remap = nr_remap << (SWP_SPECIAL_MARK);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	spaces_remap = kvcalloc(nr_remap, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces || !spaces_remap)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	for (i = 0; i < nr_remap; i++) {
		space_remap = spaces_remap + i;
		xa_init_flags(&space_remap->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space_remap->i_mmap_writable, 0);
		space_remap->a_ops = NULL;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space_remap);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;
	nr_swapper_spaces_remap[type] = nr_remap;
	swapper_spaces_remap[type] = spaces_remap;
	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	int i;
	struct address_space *spaces = swapper_spaces[type];
	struct address_space *spaces_remap = swapper_spaces_remap[type];

	for (i = 0; i < nr_swapper_spaces[type]; i++)
		VM_WARN_ON_ONCE(!mapping_empty(&spaces[i]));
	for (i = 0; i < nr_swapper_spaces_remap[type]; i++)
		VM_WARN_ON_ONCE(!mapping_empty(&spaces_remap[i]));
	kvfree(spaces);
	kvfree(spaces_remap);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
	nr_swapper_spaces_remap[type] = 0;
	swapper_spaces_remap[type] = NULL;
}

static void swap_ra_info(struct vm_fault *vmf,
			 struct vma_swap_readahead *ra_info)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, pfn, fpfn, lpfn, rpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	/*DJL ADD BEGIN*/
	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
			     SWAP_RA_ORDER_CEILING);
	// max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster), SWAP_RA_ORDER_CEILING + SWAP_RA_ORDER_CEILING_BOOST);
	/*DJL ADD END*/

	if (max_win == 1) {
		ra_info->win = 1;
		trace_new_swap_ra_info(ra_info->ptes, ra_info->nr_pte, ra_info->offset, ra_info->win);
		return;
	}

	faddr = vmf->address;
	fpfn = PFN_DOWN(faddr);
	ra_val = GET_SWAP_RA_VAL(vma);
	pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	/*DJL ADD BEGIN*/
	if (!enable_vma_readahead_boost)
		ra_info->win = win = __swapin_nr_pages(pfn, fpfn, hits,
					       max_win, prev_win);
	else
		ra_info->win = win = __swapin_nr_pages_boost(pfn, fpfn, hits,
						max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));

	/*DJL ADD BEGIN*/
	if (win == 1){
		trace_new_swap_ra_info(ra_info->ptes, ra_info->nr_pte, ra_info->offset, ra_info->win);
 		return;
	}
	// if (win == 1)
	// 	return;
	/*DJL ADD END*/

	/* Copy the PTEs because the page table may be unmapped */
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);
	if (fpfn == pfn + 1) {
		lpfn = fpfn;
		rpfn = fpfn + win;
	} else if (pfn == fpfn + 1) {
		lpfn = fpfn - win + 1;
		rpfn = fpfn + 1;
	} else {
		unsigned int left = (win - 1) / 2;

		lpfn = fpfn - left;
		rpfn = fpfn + win - left;
	}
	start = max3(lpfn, PFN_DOWN(vma->vm_start),
		     PFN_DOWN(faddr & PMD_MASK));
	end = min3(rpfn, PFN_DOWN(vma->vm_end),
		   PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));

	ra_info->nr_pte = end - start;
	ra_info->offset = fpfn - start;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte++;
#endif
	trace_new_swap_ra_info(ra_info->ptes, ra_info->nr_pte, ra_info->offset, ra_info->win);
	pte_unmap(orig_pte);
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @fentry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whose
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct page *swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask,
				       struct vm_fault *vmf, int* try_free_entry)
{
	struct blk_plug plug;
	struct blk_plug plug_save;
	struct swap_iocb *splug = NULL;
	struct swap_iocb *splug_save = NULL;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	struct mem_cgroup *memcg;
	struct lruvec * lruvec;
	struct lru_gen_folio *lrugen;
	/*DJL ADD BEGIN*/
	struct swap_info_struct *si;
	/*DJL ADD END*/
	pte_t *pte, pentry;
	swp_entry_t entry, saved_entry, mig_entry;
	bool entry_retry_putback;
	bool save_slot_finish;
	unsigned int i;
	long tmp;
	bool page_allocated ,no_space_force_stop = false;
	swp_entry_t ori_pri_entry;
	struct folio* folio_list_wb[SWAP_SLOTS_SCAN_SAVE_ONCE];
	int num_folio_list_wb = 0;

	int try_free, prio_ori, prio_mig, entry_saved, err;
	*try_free_entry = 1;
	struct vma_swap_readahead ra_info = {
		.win = 1,
	};
	struct swap_iocb *plug_save_wb = NULL;

	swap_ra_info(vmf, &ra_info);
	// ra_info.win = 1;
	lruvec = NULL;
	lrugen = NULL;
	if (ra_info.win == 1)
		goto skip_ra_try_save;
// 	if (swp_entry_test_special(fentry)){
// #ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
// 		pr_info("vma_readahead saved entry[%lx], skip", fentry.val);
// 		// dump_stack();
// #endif
// 		goto skip;
// 	}
	blk_start_plug(&plug);
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte;
	     i++, pte++) {
		pentry = *pte;
		if (!is_swap_pte(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		// // fast swap device IO-through
		// si = swp_swap_info(entry);
		// if (data_race(si->flags & SWP_SYNCHRONOUS_IO)){ // block all IOs
		// 	count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST_RA_SKIP);
		// 	continue;
		// }
		/*DJL ADD BEGIN*/
		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf->address, vmf->real_address, &page_allocated, (!enable_ra_fast_evict) || (i == ra_info.offset), 
						   &try_free, true);//, (((vmf->address >> PAGE_SHIFT) + (i - ra_info.offset))<<PAGE_SHIFT));
		/*DJL ADD END*/
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false, &splug);
			if (i != ra_info.offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
			}
			else{
				memcg = vma->vm_mm->lru_gen.memcg;
				if (memcg)
					lruvec = mem_cgroup_lruvec(memcg, folio_pgdat(page_folio(page)));
				if (lruvec)
					lrugen = &lruvec->lrugen;
			}
			/*DJL ADD BEGIN*/
			si = get_swap_device(entry);
			trace_readahead_swap_readpage(page_folio(page), si);
			if (swap_info_is_fastest(si)){
				count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST);
			} 
			else if (swap_info_is_slowest(si)){
				count_memcg_event_mm(vma->vm_mm, SWAPIN_SLOW);
			} 
			else{
				count_memcg_event_mm(vma->vm_mm, SWAPIN_MID);
			}
			put_swap_device(si);
			si = NULL;

			if (i == ra_info.offset){
				*try_free_entry = try_free;
			}
			/*DJL ADD END*/
		}
		put_page(page);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();
skip_ra_try_save:
	// pr_err("swap_vma_readahead fentry[%lx]", fentry.val);
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR
	//try do serveral stale entry save here
	if (!lruvec || !lrugen)
		goto skip;
	save_slot_finish = true;
	entry_saved = 0;
	saved_entry = get_next_saved_entry(&save_slot_finish);
	if (!saved_entry.val || non_swap_entry(saved_entry)){ // can't provide now
		if (!save_slot_finish){
			pr_err("shouldn't happen. bad save");
			BUG();
		}
		// pr_err("swap_vma_readahead fentry[%lx], skip", fentry.val);
		goto skip;
		//scan slot empty now
	} 
	else{ //saved_entry has to be valid
		struct folio *folio = NULL, *next = NULL;
		int num_moved = 0;
		bool reset_private = false;
		struct swap_info_struct* p = NULL;
		entry_retry_putback = false;
		blk_start_plug(&plug_save);
		do { //continue
			int _err;
			reset_private = false;
			folio = next = NULL;
	
			MULTISWAP_MIG_INFO("start dealing with saved entry[%lx]", saved_entry.val);
			if (unlikely(non_swap_entry(saved_entry))){
				goto skip_this_save;
			}
			//check if this is usable
			p = get_swap_device(saved_entry); //lock saved device
			if (!p){
				pr_err("saved_entry[%lx] is not used anymore", saved_entry.val);
				goto skip_this_save;
			}
			if (!data_race(p->flags & SWP_SYNCHRONOUS_IO) ||
		    	!(__swap_count(saved_entry) == 1)) {
				MULTISWAP_MIG_INFO("entry[%lx] sync[%d] cnt[%d] abandoned", 
					saved_entry.val, data_race(p->flags & SWP_SYNCHRONOUS_IO) > 0, __swap_count(saved_entry));
				goto skip_this_save;
			}

			//valid entry, we can swapin
			if (unlikely(swp_entry_test_special(saved_entry))){
				pr_err("swap entry [%lx] got but special bit used !", saved_entry.val);
				goto skip_this_save;
			}
			si = get_swap_device(saved_entry);
			//test if folio is already in swapcache
			folio = raw_swap_cache_get_folio(si, saved_entry);
			if (folio) {	
				MULTISWAP_MIG_INFO("saved_entry[%lx] raw_swap_cache_get_folio folio[%p] already dealed with",
					saved_entry.val, folio);
				put_swap_device(si);
				si = NULL;
				folio_ref_dec(folio);
				goto skip_this_save;
			}
			put_swap_device(si);
			si = NULL;

			MULTISWAP_MIG_INFO("saved_entry[%lx] dealed with",saved_entry.val);
			folio = vma_alloc_folio(gfp_mask, 0,
						vma, vmf->address, false); //we don't support derect recalim so it may fail
			if (!folio) {
				pr_err("saved_entry[%lx] fail alloc folio", saved_entry.val);
				entry_retry_putback = true;
				no_space_force_stop = true;
				goto skip_this_save;
			}
			page = &folio->page;

			//folio alloc ok
			__folio_set_locked(folio);
			SetPageStaleSaved(page);
			__folio_set_swapbacked(folio);

			/* MULTISWAP:
			 * We DONT mem_cgroup_swapin_charge_swap HERE. 
			 * We just borrow some pages from system, for
			 * swap-page migration. These pages (at most XXX)
			 * will be freed soon after migration completion.
			 */
			
			/* MULTISWAP: reset page->private and trigger swap_readpage */
			folio_set_swap_entry(folio, saved_entry);			
			MULTISWAP_MIG_INFO("swap_readpage start read page[%p] ref[%d]", page, folio_ref_count(folio));
			swap_readpage(page, true, &splug_save);
			MULTISWAP_MIG_INFO("swap_readpage finished page[%p] ref[%d]", page, folio_ref_count(folio));
			count_memcg_event_mm(vma->vm_mm, SWAPIN_FAST_SAVE);
			
			if (!folio_trylock(folio))
				goto fail_page_out;

			VM_BUG_ON_FOLIO(!(page_private(folio_page(folio, 0)) == saved_entry.val), folio);

			/* 
			 * MULTISWAP: 
			 * Alloc migration destination swap page. 
			 * The swap-migration should be a transfer attempt:
			 * FROM a SWP_SYNCHRONOUS_IO swap TO a ASYNC_IO swap.
			 */
			mig_entry = folio_alloc_swap(folio, &tmp, true, false);
			if (!mig_entry.val || (mig_entry.val > LONG_MAX) 
					|| !data_race(p->flags & SWP_SYNCHRONOUS_IO) 
					|| (swp_swap_info(mig_entry)->flags & SWP_SYNCHRONOUS_IO)){ //have to xa_mk_value
				pr_err("invalid mig_entry[%lx] alloc swap", mig_entry.val);
				goto fail_page_out;
			}

			// if (swap_duplicate(mig_entry) < 0){
			// 	pr_err("fail dup mig_entry[%lx]", mig_entry.val);
			// 	goto fail_page_out;
			// }
			VM_BUG_ON_FOLIO(swap_duplicate(mig_entry) < 0, folio);

			MULTISWAP_MIG_INFO("folio_alloc_swap entry[%lx] v[%d] for folio[%p]ref[%d]cnt[%d]",
				mig_entry.val, swp_entry_test_special(mig_entry), 
				folio, folio_ref_count(folio), __swap_count(mig_entry));

			/* MULTISWAP:
			 * We just loaded in the saved page and locked it.
			 * So this page is now considered "in swap cache"
			 * We added this page to swap cache of saved swap_entry
			 */
			_err = add_to_swap_cache_save_check(folio, saved_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN), true);
			if (unlikely(_err)) {
				MULTISWAP_MIG_ERR("folio[%p] add_to_sw$_sc fail [%d] interupted", folio, _err);
				// if (_err){
				// 	pr_err("folio[%p] add_to_sw$_save_check interupted [%d]", folio, _err);
				// 	folio_ref_sub(folio, folio_nr_pages(folio));
				// 	//this page was already in swap $
				// 	goto fail_page_out;
				// }
				BUG();
				// if (__swp_swapcount(saved_entry)){
				// 	put_swap_folio(folio, saved_entry);
				// 	goto fail_page_out;					
				// }
				// else{
				// 	put_swap_folio(folio, saved_entry);
				// 	folio_unlock(folio);
				// 	goto fail_delete_saved_cache; //delete saved cache again
				// }
			}
			MULTISWAP_MIG_INFO("folio[%p] saved[%lx]cnt[%d] ref[%d] add_to_sw$_sc success ", 
				folio, saved_entry.val, __swp_swapcount(saved_entry), folio_ref_count(folio));

			/* MULTISWAP:
			 * Need move this page to the target swap, which is
			 * supposed to be a ASYNC_IO swap. ASYNC_IO swaps
			 * has to used swap cache for ASYNC writeback management. 
			 * Add this page to target swap cache.
			 */
			_err = add_to_swap_cache_save_check(folio, mig_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN), false);
			if (_err) {
				MULTISWAP_MIG_ERR("folio[%p] add_to_sw$_sc fail", folio);
				put_swap_folio(folio, mig_entry);
				folio_unlock(folio);
				goto fail_delete_saved_cache;
			}
			MULTISWAP_MIG_INFO("folio[%p] mig[%lx]cnt[%d] ref[%d] add_to_sw$_sc success ", 
				folio, mig_entry.val, __swp_swapcount(mig_entry), folio_ref_count(folio));
			//first mark entry as faked for now (currently under initialization)
			swp_entry_set_unready(&mig_entry);
			//adding a remap from saved_entry -> mig_entry
			//this is the first time mig_entry got into remap, and its irq locked
			//we don't care about if it is locked
			err = add_swp_entry_remap(folio, saved_entry, mig_entry, gfp_mask & (__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN));
			if (err){
				MULTISWAP_MIG_ERR("folio[%p] add_swp_entry_remap fail", folio);
				// folio_unlock(folio);
				goto fail_delete_mig_cache;
			}
			if (unlikely(!__swp_swapcount(saved_entry))){
				MULTISWAP_MIG_ERR("[rare] folio[%p] __swp_swapcount fail", folio);
				swp_entry_t __mig_entry;
				__mig_entry.val = mig_entry.val;
				swp_entry_clear_ext(&__mig_entry, 0x3); // 局部变量无所谓
				delete_from_swap_remap(folio, saved_entry, __mig_entry, false);
				swp_entry_clear_ext(&mig_entry, 0x3);
				delete_from_swap_cache_mig(folio, mig_entry, true, false); //page private is also cleared
				MULTISWAP_MIG_ERR("fail_delete_mig_cache folio[%p] ref[%d]", folio, folio_ref_count(folio));
				goto fail_page_out;
				goto fail_delete_mig_cache;
			}
			if (unlikely(!folio_test_stalesaved(folio))){
				MULTISWAP_MIG_ERR("folio[%p] got intercepted before remap init", folio);
				swp_entry_t __mig_entry;
				__mig_entry.val = mig_entry.val;
				swp_entry_clear_ext(&__mig_entry, 0x3); // 局部变量无所谓
				delete_from_swap_remap(folio, saved_entry, __mig_entry, false);
				swp_entry_clear_ext(&mig_entry, 0x3);
				delete_from_swap_cache_mig(folio, mig_entry, true, false); //page private is also cleared
				MULTISWAP_MIG_ERR("fail_delete_mig_cache folio[%p] ref[%d]", folio, folio_ref_count(folio));
				goto fail_page_out;
			}
			MULTISWAP_MIG_INFO("add_swp_remap folio[%p] ref[%d][%lx]cnt[%d]=>[%lx][%d]",
				folio, folio_ref_count(folio), 	saved_entry.val, 
				__swp_swapcount(saved_entry), mig_entry.val, __swp_swapcount(mig_entry));
			swp_entry_clear_ext(&mig_entry, 0x3); // 局部变量无所谓
			//now we add the real entry
			VM_WARN_ON_FOLIO(!folio_test_locked(folio), folio);

			//set private of folio & trigger 
			ori_pri_entry.val = page_private(folio_page(folio, 0));
			set_page_private(folio_page(folio, 0), mig_entry.val);

			//do async pageout
			VM_WARN_ON_FOLIO(!folio_mapping(folio), folio);
			folio_mark_dirty(folio); //to force wb
			try_to_unmap_flush_dirty();
			/*
			 * Folio is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after I/O
			 * starts and then write it out here.
			 */
			if (folio_mapping(folio)){
				switch (pageout_save(folio, folio_mapping(folio), &plug_save_wb)) {
				case 0: //PAGE_KEEP
					MULTISWAP_MIG_INFO("pageout returns PAGE_KEEP folio[%p] ref[%d]",folio, folio_ref_count(folio));
					folio_unlock(folio);
					folio_put(folio);
					reset_private = true;
					goto skip_this_save;
				case 1: //PAGE_ACTIVATE
					MULTISWAP_MIG_INFO("pageout returns PAGE_ACTIVATE");
					BUG();
					goto fail_page_out;
				case 2: //PAGE_SUCCESS
					//check if sync io
					if (folio_test_dirty(folio)){ //unexpected
						MULTISWAP_MIG_INFO("PAGE_SUCCESS dirty folio[%p]", folio);
						BUG();
					}					
					if (folio_test_writeback(folio)){//sync, under wb	
						folio_list_wb[num_folio_list_wb++] = folio; 	
						//check later in vmscan
					}
					else{ //A synchronous write - probably a ramdisk.
						; //should remove mapping and clean & free it
						MULTISWAP_MIG_INFO("not inplemented [%s:%d] folio[%p] -> pageout -> entry[%lx]",
								 __FILE__, __LINE__, folio, mig_entry.val);
						BUG();
					}
					goto scceed_pageout;
				case 3: //PAGE_CLEAN
					MULTISWAP_MIG_INFO("pageout returns PAGE_CLEAN");
				}
fail_delete_mig_cache:
				swp_entry_clear_ext(&mig_entry, 0x3);
				delete_from_swap_cache_mig(folio, mig_entry, true, false); //page private is also cleared
				MULTISWAP_MIG_ERR("fail_delete_mig_cache folio[%p] ref[%d]", folio, folio_ref_count(folio));
fail_delete_saved_cache:
				delete_from_swap_cache_mig(folio, saved_entry, true, false); //page private is also cleared
				MULTISWAP_MIG_ERR("fail_delete_saved_cache folio[%p] ref[%d]", folio, folio_ref_count(folio));
				swap_free_mig(mig_entry);
fail_page_out:
				folio_clear_dirty(folio);
				folio_clear_stalesaved(folio);
				folio_unlock(folio);
				folio_put(folio);
				MULTISWAP_MIG_INFO("fail page_out folio[%p] ref[%d], mig_entry[%lx]", 
					folio, folio_ref_count(folio),mig_entry.val);
				goto skip_this_save;
scceed_pageout:
				;
				reset_private = true;
			}
			//success triggered a swapout to slow for migration
			count_memcg_event_mm(vma->vm_mm, SWAPOUT_SLOW_SAVE);

			//set back private just for progressing test
			si = get_swap_device(saved_entry);
			prio_ori = si->prio;
			put_swap_device(si);
			si = get_swap_device(mig_entry);
			prio_mig = si->prio;
			put_swap_device(si);
			si = NULL;

			trace_vma_ra_save_stale(saved_entry.val, mig_entry.val, entry_saved, prio_ori, prio_mig);
			//end dealing with current entry_saved
			entry_saved ++ ;
			folio_set_stalesaved(folio);
			if (folio_test_locked(folio)){
				MULTISWAP_MIG_INFO("folio[%p] unlock after pageout_save", folio);
				folio_unlock(folio);
			}
skip_this_save:
			if (reset_private){ 
				//we can change it back , to maintain correctness befor bio complete, 
				// because bio has been submitted, doesn't need it(private = migentry) anymore
				set_page_private(folio_page(folio, 0), ori_pri_entry.val);
				MULTISWAP_MIG_INFO("reset folio[%p]lru[%d] ori[%lx]cnt[%d] pri[%lx]", 
					folio, folio_test_lru(folio), ori_pri_entry.val, 
					__swap_count(ori_pri_entry), page_private(folio_page(folio, 0)));
			}
			if (entry_retry_putback){
				putback_last_saved_entry(saved_entry);
				MULTISWAP_MIG_ERR("putback entry[%lx]", saved_entry.val);
			}

			if (p)
				put_swap_device(p);

			//get next
			if (entry_saved >= SWAP_SLOTS_SCAN_SAVE_ONCE)
				break;
			if (no_space_force_stop){
				MULTISWAP_MIG_INFO("stopped by no_space_force_stop");
				break;		
			}
			saved_entry = get_next_saved_entry(&save_slot_finish);
		} while (!save_slot_finish);

		if (plug_save_wb)
			swap_write_unplug(plug_save_wb);
		
		// if (non_swap_entry(saved_entry) && save_slot_finish){
		// 	//restart load
		// 	reenable_scan_cpu();
		// }

		blk_finish_plug(&plug_save);
		if (splug_save)
			swap_read_unplug(splug_save);
		//add to lru_gen
		if (lruvec && lrugen){
// #ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
// 			pr_info("try get lruvec[%p]->lock[%p]", lruvec, &lruvec->lru_lock);
// #endif
			spin_lock_irq(&lruvec->lru_lock);
			for (int i = 0; i < num_folio_list_wb; i++){
				struct folio * folio;
				num_moved++;
				if (num_moved > num_folio_list_wb){
					pr_err("num_moved >= entry_saved err");
					break;	
				}
				folio = folio_list_wb[i];//lru_to_folio_next(&folio_list_wb);
				// VM_WARN_ON_FOLIO(!folio_test_writeback(folio), folio);
				MULTISWAP_MIG_INFO("folio[%p] => saved_folios ref[%d]", folio, folio_ref_count(folio));
				// VM_WARN_ON_FOLIO(folio_ref_count(folio) != 3, folio);
				list_add(&folio->lru, &lrugen->saved_folios);
				trace_add_to_lruvec_saved_folios(lruvec, folio, num_moved);	
			}
			spin_unlock_irq(&lruvec->lru_lock);
		}
		else{
			struct folio* folio = NULL;
			pr_err("there's no lruvec[%p], unlock all folio in list", lruvec);
			BUG();
			for (int i = 0; i < num_folio_list_wb; i++){
				folio = folio_list_wb[i];
				folio_unlock(folio);
			}
		}
	}
#endif
	lru_add_drain();
skip:
	/* The page was likely read above, so no need for plugging here */
	return read_swap_cache_async(fentry, gfp_mask, vma, vmf->address,
				     ra_info.win == 1, NULL, true, try_free_entry);
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct page *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf, int* try_free_entry)
{
	return swap_use_vma_readahead() ?
			swap_vma_readahead(entry, gfp_mask, vmf, try_free_entry) :
			swap_cluster_readahead(entry, gfp_mask, vmf, try_free_entry);
}

void swap_scan_save_entries(swp_entry_t *entries, int n){
	return;
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  enable_vma_readahead ? "true" : "false");
}
static ssize_t swap_scan_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  swap_scan_enabled_sysfs ? "true" : "false");
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &enable_vma_readahead);
	if (ret)
		return ret;

	return count;
}
static ssize_t swap_scan_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &swap_scan_enabled_sysfs);
	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);
static struct kobj_attribute swap_scan_enabled_attr = __ATTR_RW(swap_scan_enabled);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	&swap_scan_enabled_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif

/* MULTISWAP FIX: Immediate shadow cleanup to prevent slot reuse races */
void cleanup_shadow_entry_immediate(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	void *shadow;
	
	if (!address_space)
		return;
	
	xa_lock_irq(&address_space->i_pages);
	shadow = xa_load(&address_space->i_pages, idx);
	if (shadow && entry_is_entry_ext(shadow) == 1) {
		/* Remove shadow entry immediately to prevent slot reuse */
		xa_store(&address_space->i_pages, idx, NULL, GFP_ATOMIC);
		/* MULTISWAP FIX: Count shadow entry cleanup */
		atomic_long_inc(&shadow_entries_cleaned);
		shadow_entry_free(shadow);
	}
	xa_unlock_irq(&address_space->i_pages);
}
