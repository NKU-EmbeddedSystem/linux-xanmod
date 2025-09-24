/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM lru_gen

#if !defined(_TRACE_LRU_GEN_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_LRU_GEN_H

#include <linux/tracepoint.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/ktime.h>
#define	PAGEMAP_MAPPED		0x0001u
#define PAGEMAP_ANONYMOUS	0x0002u
#define PAGEMAP_FILE		0x0004u
#define PAGEMAP_SWAPCACHE	0x0008u
#define PAGEMAP_SWAPBACKED	0x0010u
#define PAGEMAP_MAPPEDDISK	0x0020u
#define PAGEMAP_BUFFERS		0x0040u

#define trace_pagemap_flags(folio) ( \
	(folio_test_anon(folio)		? PAGEMAP_ANONYMOUS  : PAGEMAP_FILE) | \
	(folio_mapped(folio)		? PAGEMAP_MAPPED     : 0) | \
	(folio_test_swapcache(folio)	? PAGEMAP_SWAPCACHE  : 0) | \
	(folio_test_swapbacked(folio)	? PAGEMAP_SWAPBACKED : 0) | \
	(folio_test_mappedtodisk(folio)	? PAGEMAP_MAPPEDDISK : 0) | \
	(folio_test_private(folio)	? PAGEMAP_BUFFERS    : 0) \
	)

TRACE_EVENT(mglru_folio_updt_gen,

	TP_PROTO(struct folio *folio, int oldgen, int newgen),

	TP_ARGS(folio, oldgen, newgen),

	TP_STRUCT__entry(
		__field(struct folio *,	folio	)
		__field(unsigned long,	pfn	)
		__field(int , oldgen    )
		__field(int , newgen    )
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->pfn	= folio_pfn(folio);
		__entry->oldgen	= oldgen;
		__entry->newgen	= newgen;
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio=%p[%s] pfn=0x%lx gen:%d->%d flags=%s%s%s%s%s%s",
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			__entry->pfn,
			__entry->oldgen,
			__entry->newgen,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);

TRACE_EVENT(mglru_folio_inc_gen,

	TP_PROTO(struct folio *folio, int oldgen, int newgen),

	TP_ARGS(folio, oldgen, newgen),

	TP_STRUCT__entry(
		__field(struct folio *,	folio	)
		__field(unsigned long,	pfn	)
		__field(int , oldgen    )
		__field(int , newgen    )
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->pfn	= folio_pfn(folio);
		__entry->oldgen	= oldgen;
		__entry->newgen	= newgen;
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio=%p[%s] pfn=0x%lx gen:%d->%d flags=%s%s%s%s%s%s",
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			__entry->pfn,
			__entry->oldgen,
			__entry->newgen,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);


TRACE_EVENT(mglru_sort_folio,

	TP_PROTO(struct lruvec *lruvec, struct folio *folio, int ref, int tier, int reason),

	TP_ARGS(lruvec, folio, ref, tier, reason),

	TP_STRUCT__entry(
		__field(struct lruvec *, lruvec	)
		__field(struct folio *,	folio	)
		__field(unsigned long,	pfn	)
		__field(int , tier    )
		__field(int , reason    )
		__field(int, ref)
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->lruvec	= lruvec;
		__entry->folio	= folio;
		__entry->pfn	= folio_pfn(folio);
		__entry->ref    = ref;
		__entry->tier   = tier;
		__entry->reason	= reason;
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("[%s] folio=%p[%s][ra%d] ref[%d] pfn=0x%lx gen:%d tier=%d flags=%s%s%s%s%s%s",
			(__entry->reason == 0 )? "KILLED" : (
			(__entry->reason == 1 )? "unevictable" : (
			(__entry->reason == 2 )? "dirty lazyfree" : (
			(__entry->reason == 3 )? "promoted" : (
			(__entry->reason == 4 )? "protected" : "writeback" 
			)))),
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			folio_test_readahead(__entry->folio),
			__entry->ref,
			__entry->pfn,
			folio_lru_gen(__entry->folio),
			__entry->tier,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);

TRACE_EVENT(mglru_isolate_folio,

	TP_PROTO(struct lruvec *lruvec, struct folio *folio, 
	         int orirefs, int newrefs, int oritiers, int newtiers, int gen),

	TP_ARGS(lruvec, folio, orirefs, newrefs, oritiers, newtiers, gen),

	TP_STRUCT__entry(
		__field(struct lruvec *, lruvec	)
		__field(struct folio *,	folio	)
		__field(unsigned long,	pfn	)
		__field(int , orirefs    )
		__field(int , newrefs    )
		__field(int , oritiers    )
		__field(int , newtiers    )
		__field(int , gen    )
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->lruvec	= lruvec;
		__entry->folio	= folio;
		__entry->pfn	= folio_pfn(folio);
		__entry->orirefs	= orirefs;
		__entry->newrefs	= newrefs;
		__entry->oritiers	= oritiers;
		__entry->newtiers	= newtiers;
		__entry->gen = gen;
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio=%p[%s] pfn=0x%lx gen:%d refs:%d->%d tiers:%d->%d flags=%s%s%s%s%s%s",
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			__entry->pfn,
			__entry->gen,
			__entry->orirefs,
			__entry->newrefs,
			__entry->oritiers,
			__entry->newtiers,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);

TRACE_EVENT(walk_pte_range,

	TP_PROTO(struct folio *folio, 	unsigned long addr, int old_gen, int new_gen, int old_refs),

	TP_ARGS(folio, addr, old_gen, new_gen, old_refs),

	TP_STRUCT__entry(
		__field(struct folio *,	folio	)
		__field(unsigned long,	addr	)
		__field(int , type    )
		__field(int , oldgen    )
		__field(int , newgen    )
		__field(int,  new_refs		)
		__field(int,  old_refs		)
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->addr	= addr;
		__entry->oldgen	= old_gen;
		__entry->newgen	= new_gen;
		__entry->new_refs	= folio_lru_refs(folio);
		__entry->old_refs   = old_refs;
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio=%p[%s] addr=0x%lx  gen:%d->%d  refs=%d->%d, flags=%s%s%s%s%s%s",
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			__entry->addr,
			__entry->oldgen,
			__entry->newgen,
			__entry->old_refs,
			__entry->new_refs,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);

TRACE_EVENT(folio_update_gen,

	TP_PROTO(struct folio *folio, int newgen, int type),

	TP_ARGS(folio, newgen, type),

	TP_STRUCT__entry(
		__field(struct folio *,	folio	)
		__field(unsigned long,	pfn	)
		__field(int , type    )
		__field(int , oldgen    )
		__field(int , newgen    )
		__field(unsigned long,	flags	)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->pfn	= folio_pfn(folio);
		__entry->oldgen	= ((READ_ONCE(folio->flags) & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
		__entry->newgen	= newgen;
		__entry->type	= type;
		__entry->flags	= trace_pagemap_flags(folio);
	),

	/* Flag format is based on page-types.c formatting for pagemap */
	TP_printk("folio=%p[%s] pfn=0x%lx type:%s gen:%d->%d flags=%s%s%s%s%s%s",
			__entry->folio,
			folio_test_transhuge(__entry->folio)? "T": "N",
			__entry->pfn,
			__entry->type == 0 ? "other" : (__entry->type == 1 ? "norm" : "thp"),
			__entry->oldgen,
			__entry->newgen,
			__entry->flags & PAGEMAP_MAPPED		? "M" : " ",
			__entry->flags & PAGEMAP_ANONYMOUS	? "a" : "f",
			__entry->flags & PAGEMAP_SWAPCACHE	? "s" : " ",
			__entry->flags & PAGEMAP_SWAPBACKED	? "b" : " ",
			__entry->flags & PAGEMAP_MAPPEDDISK	? "d" : " ",
			__entry->flags & PAGEMAP_BUFFERS	? "B" : " ")
);

TRACE_EVENT(page_set_swapprio,

	TP_PROTO(struct page* page),

	TP_ARGS(page),

	TP_STRUCT__entry(
		__field(struct page* ,page)
	),

	TP_fast_assign(
		__entry->page	= page;
	),

	TP_printk("page@[%p] lowprio[%d],highprio[%d]", 
                __entry->page,
				PageSwapPrioHigh(__entry->page),
                PageSwapPrioLow(__entry->page)
                )
);

TRACE_EVENT(folio_set_swapprio,

	TP_PROTO(struct folio* folio),

	TP_ARGS(folio),

	TP_STRUCT__entry(
		__field(struct folio* , folio)
	),

	TP_fast_assign(
		__entry->folio	= folio;
	),

	TP_printk("folio@[%p] highp[%d],lowp[%d]", 
                __entry->folio,
                folio_test_swappriohigh(__entry->folio),
                folio_test_swappriolow(__entry->folio))
);

TRACE_EVENT(refill_swap_slots,

	TP_PROTO(int type, int nr, signed short prio),

	TP_ARGS(type, nr , prio),

	TP_STRUCT__entry(
		__field(int ,type)
		__field(int ,nr)
		__field(signed short ,prio)
	),

	TP_fast_assign(
		__entry->type	= type;
		__entry->nr	= nr;
		__entry->prio	= prio;
	),

	TP_printk("refill[%s] nr[%d],prio[%d]", 
                (__entry->type == 0) ? "slots" : (
				(__entry->type == 1) ? "slots_slow" : "slots_fast"),
                __entry->nr,
                __entry->prio)
);

TRACE_EVENT(swap_scan_change_state, 
	TP_PROTO(int type, long left, long total),

	TP_ARGS(type, left , total),

	TP_STRUCT__entry(
		__field(int ,type)
		__field(long ,left)
		__field(long ,total)
	),

	TP_fast_assign(
		__entry->type	= type;
		__entry->left	= left;
		__entry->total	= total;
	),

	TP_printk("state[%s][%d] left[%ld],totel[%ld]", 
                (__entry->type == 0) ? "deactivate" : (
				(__entry->type == 1) ? "activate" : "unknown"),
				__entry->type,
                __entry->left,
                __entry->total)
);

TRACE_EVENT(add_to_scan_slot, 
	TP_PROTO(int cpu, int cur, int nr),

	TP_ARGS(cpu, cur , nr),

	TP_STRUCT__entry(
		__field(int ,cpu)
		__field(int ,cur)
		__field(int ,nr)
	),

	TP_fast_assign(
		__entry->cpu	= cpu;
		__entry->cur	= cur;
		__entry->nr	= nr;
	),

	TP_printk("cpu[%d] cur[%d/%d]", 
                __entry->cpu,
				__entry->cur,
                __entry->nr)
);

TRACE_EVENT(scan_entries_savior, 
	TP_PROTO(int memcg_id, unsigned long old_seq, unsigned long min_seq, int threshold),

	TP_ARGS(memcg_id, old_seq , min_seq, threshold),

	TP_STRUCT__entry(
		__field(int ,memcg_id)
		__field(unsigned long ,old_seq)
		__field(unsigned long ,min_seq)
		__field(int ,threshold)
	),

	TP_fast_assign(
		__entry->memcg_id	= memcg_id;
		__entry->old_seq	= old_seq;
		__entry->min_seq	= min_seq;
		__entry->threshold	= threshold;
	),

	TP_printk("seq[%ld]-seq[%ld]=[%ld] threshold[%d],memcg[%d]", 
                __entry->min_seq,
				__entry->old_seq,
				__entry->min_seq - __entry->old_seq,
				__entry->threshold,
                __entry->memcg_id)
);

TRACE_EVENT(swap_shadow_scan_next, 
	TP_PROTO(unsigned int start, unsigned int end, int scanned, int memcg_id),

	TP_ARGS(start, end , scanned, memcg_id),

	TP_STRUCT__entry(
		__field(unsigned int ,start)
		__field(unsigned int ,end)
		__field(int ,scanned)
		__field(int ,memcg_id)
	),

	TP_fast_assign(
		__entry->start	= start;
		__entry->end	= end;
		__entry->scanned	= scanned;
		__entry->memcg_id	= memcg_id;
	),

	TP_printk("offset[%d-%d] scanned[%d],memcg[%d]", 
                __entry->start,
				__entry->end,
				__entry->scanned,
                __entry->memcg_id)
);

TRACE_EVENT(folio_delete_from_swap_cache,

	TP_PROTO(struct folio* folio, int refs, int prio),

	TP_ARGS(folio, refs, prio),

	TP_STRUCT__entry(
		__field(struct folio* ,folio)
		__field(int , refs)
		__field(int , prio)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->refs	= refs;
		__entry->prio	= prio;
	),

	TP_printk("folio@[%p] fdfsc_ra[%d]=>prio[%d] gen[%d] refs[%d]", 
                __entry->folio, folio_test_readahead(__entry->folio), __entry->prio, folio_lru_gen(__entry->folio), __entry->refs)
);
#ifdef CONFIG_LRU_GEN_KEEP_REFAULT_HISTORY
TRACE_EVENT(folio_lock_timer,
    TP_PROTO(unsigned long swap_entry, unsigned long mig_entry, int cpu, unsigned long wait_time_ns),
    TP_ARGS(swap_entry, mig_entry, cpu, wait_time_ns),
    TP_STRUCT__entry(
		__field(unsigned long, swap_entry)
		__field(unsigned long, mig_entry)
        __field(int, cpu)
        __field(unsigned long, wait_time_ns)
    ),
    TP_fast_assign(
		__entry->swap_entry	= swap_entry;
		__entry->mig_entry  = mig_entry;
		__entry->cpu = cpu;
        __entry->wait_time_ns = wait_time_ns;
    ),
    TP_printk("entry=[%lx] mig=[%lx] cpu=%d wait_time_ns=%lu", __entry->swap_entry, __entry->mig_entry, __entry->cpu, __entry->wait_time_ns)
);

TRACE_EVENT(folio_fail_lock_timer,
    TP_PROTO(unsigned long swap_entry, unsigned long mig_entry, int cpu, unsigned long wait_time_ns),
    TP_ARGS(swap_entry, mig_entry, cpu, wait_time_ns),
    TP_STRUCT__entry(
		__field(unsigned long, swap_entry)
		__field(unsigned long, mig_entry)
        __field(int, cpu)
        __field(unsigned long, wait_time_ns)
    ),
    TP_fast_assign(
		__entry->swap_entry	= swap_entry;
		__entry->mig_entry  = mig_entry;
        __entry->cpu = cpu;
        __entry->wait_time_ns = wait_time_ns;
    ),
    TP_printk("fail entry=[%lx] mig=[%lx] cpu=%d wait_time_ns=%lu", __entry->swap_entry,  __entry->mig_entry, __entry->cpu, __entry->wait_time_ns)
);


TRACE_EVENT(shadow_ext_transfer,

	TP_PROTO(struct folio* folio, 
			 unsigned short cgroup_id, 
			 struct shadow_entry* se_new, 
			 struct shadow_entry* se_old, 
			 unsigned long swap_entry),

	TP_ARGS(folio, cgroup_id, se_new, se_old, swap_entry),

	TP_STRUCT__entry(
		__field(struct folio* ,folio)
		__field(unsigned short, cgroup_id)
		__field(struct shadow_entry* , se_new)
		__field(struct shadow_entry* , se_old)
		__field(unsigned long , swap_entry)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->cgroup_id	= cgroup_id;
		__entry->se_new	= se_new;
		__entry->se_old	= se_old;
		__entry->swap_entry	= swap_entry;
	),

	TP_printk("entry[%lx] folio@[%lx]{memcg:%d} se{%p}[cnt:%d,avg:%d,evict:%d]<=se{%p}[cnt:%d,avg:%d,evict:%d]", 
				__entry->swap_entry,
				(((unsigned long)(__entry->folio)) & (0xffffffffffff)), 
				(unsigned short)__entry->cgroup_id,
				__entry->se_new,
				// __entry->se_new->memcg_id, 
				__entry->se_new->hist_ts[SE_HIST_REFAULT_COUNT], 
				__entry->se_new->hist_ts[SE_HIST_AVG_DISTANCE], __entry->se_new->hist_ts[SE_HIST_PAGE_ID],
				__entry->se_old,
				// __entry->se_old->memcg_id,
				__entry->se_old->hist_ts[SE_HIST_REFAULT_COUNT],
				__entry->se_old->hist_ts[SE_HIST_AVG_DISTANCE], __entry->se_old->hist_ts[SE_HIST_PAGE_ID]
			 )
);

TRACE_EVENT(folio_ws_chg_se,

	TP_PROTO(struct folio* folio, 
		     unsigned long va,
			 unsigned short cgroup_id, 
			 unsigned long token, 
			 int refs, 
			 bool in,
			 int swap_level,
			 long swap_space_left,
			 struct shadow_entry* se, 
			 unsigned long swap_entry),

	TP_ARGS(folio, va, cgroup_id, token, refs, in, swap_level, swap_space_left, se, swap_entry),

	TP_STRUCT__entry(
		__field(struct folio* ,folio)
		__field(unsigned long , va)
		__field(unsigned short, cgroup_id)
		__field(unsigned long , token)
		__field(int , refs)
		__field(int, tiers)
		__field(bool , in)
		__field(int , swap_level)
		__field(long , swap_space_left)
		__field(struct shadow_entry* , se)
		__field(unsigned long , swap_entry)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->va	= (va >> PAGE_SHIFT);
		__entry->cgroup_id	= cgroup_id;
		__entry->token	= token;
		__entry->refs	= refs;
		__entry->tiers  = lru_tier_from_refs(refs);
		__entry->in	= in;
		__entry->swap_level	= swap_level;
		__entry->swap_space_left	= swap_space_left;
		__entry->se	= se;
		__entry->swap_entry = swap_entry;
	),

	TP_printk("[%s%s]left[%ld] entry[%lx] va[%lx]->folio@[%lx]{[%s]ra[%d]gen[%d]}\
{memcg:%d}min_seq[%lu];ref[%d];tier[%d] \
se{%p}[cnt:%d,avg:%d,page_id:%d]",
                __entry->in ? "RE<=" : "EV=>",
				__entry->swap_level == 1 ? "f" : (
				__entry->swap_level == 0 ? "m" : (
				__entry->swap_level == -1 ? "s" : "unknown"
				)),
				__entry->swap_space_left,
				__entry->swap_entry,
				__entry->va,
				(((unsigned long)(__entry->folio)) & (0xffffffffffff)),
				folio_test_swappriolow(__entry->folio) ? "s" : (
				folio_test_swappriohigh(__entry->folio) ? "f" : "m"),
				folio_test_readahead(__entry->folio),
				folio_lru_gen(__entry->folio),
				(unsigned short)__entry->cgroup_id,
				(__entry->token >> LRU_REFS_WIDTH),
				__entry->refs, __entry->tiers,
				__entry->se,
				__entry->se->hist_ts[SE_HIST_REFAULT_COUNT],
				__entry->se->hist_ts[SE_HIST_AVG_DISTANCE],
				__entry->se->hist_ts[SE_HIST_PAGE_ID])
);
#endif
TRACE_EVENT(folio_ws_chg,

	TP_PROTO(struct folio* folio, 
		     unsigned long va,
	         struct pglist_data *pgdat, 
			 unsigned short cgroup_id, 
			 unsigned long token, 
			 int refs, 
			 bool in,
			 int swap_level, 
			 long swap_space_left, 
			 unsigned long swap_entry),

	TP_ARGS(folio, va, pgdat, cgroup_id, token, refs, in, swap_level, swap_space_left, swap_entry),

	TP_STRUCT__entry(
		__field(struct folio* ,folio)
		__field(unsigned long , va)
		__field(struct pglist_data*, pgdat)
		__field(unsigned short, cgroup_id)
		__field(unsigned long , token)
		__field(int , refs)
		__field(int, tiers)
		__field(bool , in)
		__field(int , swap_level)
		__field(long , swap_space_left)
		__field(unsigned long , swap_entry)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->va	= (va >> PAGE_SHIFT);
		__entry->pgdat	= pgdat;
		__entry->cgroup_id	= cgroup_id;
		__entry->token	= token;
		__entry->refs	= refs;
		__entry->tiers  = lru_tier_from_refs(refs);
		__entry->in	= in;
		__entry->swap_entry	= swap_entry;
		__entry->swap_level	= swap_level;
		__entry->swap_space_left = swap_space_left;
	),

	TP_printk("[%s%s]left[%ld] entry[%lx] va[%lx]->folio@[%lx]{[%s]ra[%d]gen[%d]}{memcg:%d}min_seq[%lu];ref[%d];tier[%d]", 
                __entry->in ? "RE<=" : "EV=>",
				__entry->swap_level == 1 ? "f" : (
				__entry->swap_level == 0 ? "m" : (
				__entry->swap_level == -1 ? "s" : "unknown"
				)),
				__entry->swap_space_left,
				__entry->swap_entry,
				__entry->va,
				(((unsigned long)(__entry->folio)) & (0xffffffffffff)), 
				folio_test_swappriolow(__entry->folio) ? "s" : (
				folio_test_swappriohigh(__entry->folio) ? "f" : "m"),
				folio_test_readahead(__entry->folio),
				folio_lru_gen(__entry->folio),
				(unsigned short)__entry->cgroup_id,
				(__entry->token >> LRU_REFS_WIDTH),
				__entry->refs, __entry->tiers)
);
TRACE_EVENT(get_shadow_swcache,

	TP_PROTO(unsigned long swap_entry, void* old, int state),

	TP_ARGS(swap_entry, old, state),

	TP_STRUCT__entry(
		__field(unsigned long, swap_entry)
		__field(void*, old)
		__field(int, state)
	),

	TP_fast_assign(
		__entry->swap_entry	= swap_entry;
		__entry->old = old;
		__entry->state = state;
	),

	TP_printk("state[%d] entry[%lx] shadow[%p]",
			 __entry->state, __entry->swap_entry, __entry->old)
);
TRACE_EVENT(shadow_entry_free,

	TP_PROTO(void* shadow_ext, int place),

	TP_ARGS(shadow_ext, place),

	TP_STRUCT__entry(
		__field(void*, shadow_ext)
		__field(int, place)
	),

	TP_fast_assign(
		__entry->shadow_ext	= shadow_ext;
		__entry->place = place;
	),

	TP_printk("shadow_ext[%p] [%d]",
			 __entry->shadow_ext, __entry->place)
);
TRACE_EVENT(delete_swcache,

	TP_PROTO(struct folio* folio, unsigned long swap_entry, int place),

	TP_ARGS(folio, swap_entry, place),

	TP_STRUCT__entry(
		__field(struct folio*,  folio)
		__field(unsigned long, swap_entry)
		__field(int, place)
	),

	TP_fast_assign(
		__entry->swap_entry	= swap_entry;
		__entry->folio = folio;
		__entry->place = place;
	),

	TP_printk("[%d]entry[%lx] folio[%p]",
			 __entry->place, __entry->swap_entry, __entry->folio)
);

TRACE_EVENT(damon_folio_mark_accessed,

	TP_PROTO(struct folio* folio, int ref, int pa),

	TP_ARGS(folio, ref, pa),

	TP_STRUCT__entry(
		__field(struct folio* , folio)
		__field(int , ref)
		__field(int , tiers)
		__field(int , pa)
	),

	TP_fast_assign(
		__entry->folio	= folio;
		__entry->ref   = ref;
		__entry->tiers  = lru_tier_from_refs(ref);
		__entry->pa   = pa;
	),

	TP_printk("[%s] folio[%p] ref[%d] tier[%d] PG_work[%d] PG_ref[%d]",
			 __entry->pa ? "paddr" : "vaddr",  
			 __entry->folio, __entry->ref, __entry->tiers, 
			 folio_test_workingset(__entry->folio), 
			 folio_test_referenced(__entry->folio))
);

TRACE_EVENT(damon_va_check_access,

	TP_PROTO(void* r, unsigned long sampling_addr, unsigned long start, unsigned long end),

	TP_ARGS(r, sampling_addr, start, end),

	TP_STRUCT__entry(
		__field(void* ,r)
		__field(unsigned long ,sampling_addr)
		__field(unsigned long ,start)
		__field(unsigned long ,end)
	),

	TP_fast_assign(
		__entry->r	= r;
		__entry->sampling_addr	= sampling_addr;
		__entry->start	= start;
		__entry->end	= end;
	),

	TP_printk("region@[%p][%lu-%lu] len[%lu] accessed sample addr[%lu]", 
                __entry->r, __entry->start / PAGE_SIZE, __entry->end / PAGE_SIZE, (__entry->end - __entry->start) / PAGE_SIZE,  __entry->sampling_addr)
);

#endif /* _TRACE_LRU_GEN_H */
#include <trace/define_trace.h>