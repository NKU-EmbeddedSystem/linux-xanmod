/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEPILOT_OVERHEAD_H
#define _LINUX_PAGEPILOT_OVERHEAD_H
/*
 * PagePilot per-segment overhead sampling.
 *
 * Lightweight, sampled ktime instrumentation placed around PagePilot's
 * newly-added code segments so their average cost (ns) can be measured in
 * place on a single running kernel (no A/B build required).  Only 1 in
 * pp_oh_sample_n[site] calls is timed, so perturbation of the measured hot
 * path stays small.  Per-site averages are exposed at
 *
 *     /sys/kernel/debug/pagepilot_overhead    (read: table; write: reset)
 *
 * Enable with CONFIG_PAGEPILOT_OVERHEAD_STAT; keep it OFF for production /
 * for the performance numbers reported in the paper.
 */
#include <linux/types.h>

enum pp_oh_site {
	PP_OH_MIGENTRY_LOCK,	/* do_swap_page: indirect remap lookup + lock  */
	PP_OH_MIGENTRY_UNLOCK,	/* do_swap_page: indirect remap unlock         */
	PP_OH_ROUTER_DECISION,	/* folio_alloc_swap: fast/slow routing choice  */
	PP_OH_NR_SITES,
};

#ifdef CONFIG_PAGEPILOT_OVERHEAD_STAT
#include <linux/ktime.h>
#include <linux/compiler.h>

extern unsigned int pp_oh_sample_n[PP_OH_NR_SITES];
void pp_oh_record(int site, u64 ns);

/* Declare per-site timing scratch vars at function scope. */
#define PP_OH_DECL(tok)						\
	ktime_t tok##_t0 __maybe_unused;			\
	bool tok##_do __maybe_unused = false

/* Time only 1 in pp_oh_sample_n[site] calls (racy counter is fine). */
#define PP_OH_BEGIN(site, tok)					\
	do {							\
		static unsigned int tok##_cnt;			\
		if (++tok##_cnt >= pp_oh_sample_n[site]) {	\
			tok##_cnt = 0;				\
			tok##_do = true;			\
			tok##_t0 = ktime_get();			\
		}						\
	} while (0)

#define PP_OH_END(site, tok)					\
	do {							\
		if (tok##_do)					\
			pp_oh_record(site,			\
			    ktime_to_ns(ktime_sub(ktime_get(),	\
						  tok##_t0)));	\
	} while (0)
#else
#define PP_OH_DECL(tok)
#define PP_OH_BEGIN(site, tok)	do { } while (0)
#define PP_OH_END(site, tok)	do { } while (0)
#endif /* CONFIG_PAGEPILOT_OVERHEAD_STAT */

#endif /* _LINUX_PAGEPILOT_OVERHEAD_H */
