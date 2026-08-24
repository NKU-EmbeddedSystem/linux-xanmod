// SPDX-License-Identifier: GPL-2.0
/*
 * PagePilot per-segment overhead sampling (see linux/pagepilot_overhead.h).
 *
 * Accumulates sampled per-call durations of PagePilot's added code segments
 * and exposes per-site averages at /sys/kernel/debug/pagepilot_overhead.
 */
#include <linux/pagepilot_overhead.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

static atomic64_t pp_oh_sum_ns[PP_OH_NR_SITES];
static atomic64_t pp_oh_cnt[PP_OH_NR_SITES];

/*
 * Per-site sampling period: time 1 of every N calls.  Tune to call
 * frequency -- sparse for hot critical-path sites (plenty of samples per
 * run, minimal perturbation), dense for rarely-triggered ones.
 */
unsigned int pp_oh_sample_n[PP_OH_NR_SITES] = {
	[PP_OH_MIGENTRY_LOCK]	 = 10,	/* every fast-device swap-in; dense so
					 * the rare HIT outcome gets samples  */
	[PP_OH_MIGENTRY_LOCK_HIT] = 10,	/* outcome bucket; N taken from _LOCK */
	[PP_OH_MIGENTRY_UNLOCK]	 = 1,	/* hit path only (rare): sample all   */
	[PP_OH_ROUTER_DECISION]	 = 100,	/* every swap-out                     */
	[PP_OH_TRACER_EVICT]	 = 100,	/* every tracked eviction             */
	[PP_OH_REFAULT_TRACK]	 = 100,	/* every refault                      */
	[PP_OH_MIG_SCAN]	 = 1,	/* 1/256 kswapd shrinks: sample all   */
	[PP_OH_MIG_POLL]	 = 100,	/* readahead tail, ~every fault       */
	[PP_OH_MIG_ISSUE]	 = 1,	/* per migration batch: sample all    */
	[PP_OH_MIG_COMPLETE]	 = 1,	/* per reclaim pass: sample all       */
	[PP_OH_CALIB]		 = 1,
};

static const char *const pp_oh_name[PP_OH_NR_SITES] = {
	[PP_OH_MIGENTRY_LOCK]	 = "lookup_miss",
	[PP_OH_MIGENTRY_LOCK_HIT] = "lookup_hit",
	[PP_OH_MIGENTRY_UNLOCK]	 = "unlock_on_hit",
	[PP_OH_ROUTER_DECISION]	 = "router_decision",
	[PP_OH_TRACER_EVICT]	 = "tracer_evict",
	[PP_OH_REFAULT_TRACK]	 = "refault_track",
	[PP_OH_MIG_SCAN]	 = "mig_scan",
	[PP_OH_MIG_POLL]	 = "mig_poll",
	[PP_OH_MIG_ISSUE]	 = "mig_issue",
	[PP_OH_MIG_COMPLETE]	 = "mig_complete",
	[PP_OH_CALIB]		 = "empty_probe",
};

/* Full-count auxiliary gauges/counters (not sampled). */
static atomic64_t pp_remap_live;
static atomic64_t pp_remap_peak;
static atomic64_t pp_mig_candidates;
static atomic64_t pp_mig_putback;

void pp_remap_live_inc(void)
{
	s64 live = atomic64_inc_return(&pp_remap_live);
	s64 peak = atomic64_read(&pp_remap_peak);

	while (live > peak) {
		s64 old = atomic64_cmpxchg(&pp_remap_peak, peak, live);

		if (old == peak)
			break;
		peak = old;
	}
}

void pp_remap_live_dec(void)
{
	atomic64_dec(&pp_remap_live);
}

void pp_mig_candidates_inc(void)
{
	atomic64_inc(&pp_mig_candidates);
}

void pp_mig_putback_inc(void)
{
	atomic64_inc(&pp_mig_putback);
}

void pp_oh_record(int site, u64 ns)
{
	if (unlikely((unsigned int)site >= PP_OH_NR_SITES))
		return;
	atomic64_add(ns, &pp_oh_sum_ns[site]);
	atomic64_inc(&pp_oh_cnt[site]);
}

static int pp_oh_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "%-16s %12s %16s %10s %6s\n",
		   "site", "samples", "sum_ns", "avg_ns", "1/N");
	for (i = 0; i < PP_OH_NR_SITES; i++) {
		u64 c = atomic64_read(&pp_oh_cnt[i]);
		u64 s = atomic64_read(&pp_oh_sum_ns[i]);

		seq_printf(m, "%-16s %12llu %16llu %10llu %6u\n",
			   pp_oh_name[i], c, s, c ? div64_u64(s, c) : 0,
			   pp_oh_sample_n[i]);
	}
	seq_printf(m, "\nremap_live %lld\nremap_peak %lld\n"
		   "mig_candidates %lld\nmig_putback %lld\n",
		   (long long)atomic64_read(&pp_remap_live),
		   (long long)atomic64_read(&pp_remap_peak),
		   (long long)atomic64_read(&pp_mig_candidates),
		   (long long)atomic64_read(&pp_mig_putback));
	return 0;
}

static int pp_oh_open(struct inode *inode, struct file *file)
{
	return single_open(file, pp_oh_show, NULL);
}

/*
 * Measure the empty BEGIN/END window so probe cost can be reported and
 * (optionally) subtracted.  Mirrors the real probe structure exactly: the
 * timed window contains only the return from the first ktime_get() and the
 * entry into the second; the atomic accumulation stays outside it.
 */
static void pp_oh_calibrate(int iters)
{
	int i;

	atomic64_set(&pp_oh_sum_ns[PP_OH_CALIB], 0);
	atomic64_set(&pp_oh_cnt[PP_OH_CALIB], 0);
	for (i = 0; i < iters; i++) {
		ktime_t t0 = ktime_get();

		pp_oh_record(PP_OH_CALIB,
			     ktime_to_ns(ktime_sub(ktime_get(), t0)));
		if (!(i % 65536))
			cond_resched();
	}
}

/* any write resets all counters (remap_live is a gauge: kept; peak := live) */
static ssize_t pp_oh_write(struct file *file, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	int i;

	for (i = 0; i < PP_OH_NR_SITES; i++) {
		atomic64_set(&pp_oh_sum_ns[i], 0);
		atomic64_set(&pp_oh_cnt[i], 0);
	}
	atomic64_set(&pp_remap_peak, atomic64_read(&pp_remap_live));
	atomic64_set(&pp_mig_candidates, 0);
	atomic64_set(&pp_mig_putback, 0);
	pp_oh_calibrate(100000);
	return len;
}

static const struct file_operations pp_oh_fops = {
	.owner		= THIS_MODULE,
	.open		= pp_oh_open,
	.read		= seq_read,
	.write		= pp_oh_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init pp_oh_init(void)
{
	debugfs_create_file("pagepilot_overhead", 0644, NULL, NULL,
			    &pp_oh_fops);
	pp_oh_calibrate(1000000);
	return 0;
}
late_initcall(pp_oh_init);
