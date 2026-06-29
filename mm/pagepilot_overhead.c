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
#include <linux/uaccess.h>

static atomic64_t pp_oh_sum_ns[PP_OH_NR_SITES];
static atomic64_t pp_oh_cnt[PP_OH_NR_SITES];

/*
 * Per-site sampling period: time 1 of every N calls.  Tune to call
 * frequency -- sparse for hot critical-path sites (plenty of samples per
 * run, minimal perturbation), dense for rarely-triggered ones.
 */
unsigned int pp_oh_sample_n[PP_OH_NR_SITES] = {
	[PP_OH_MIGENTRY_LOCK]	= 100,	/* every fast-device swap-in */
	[PP_OH_MIGENTRY_UNLOCK]	= 100,	/* every fast-device swap-in */
	[PP_OH_ROUTER_DECISION]	= 100,	/* every swap-out            */
};

static const char *const pp_oh_name[PP_OH_NR_SITES] = {
	[PP_OH_MIGENTRY_LOCK]	= "migentry_lock",
	[PP_OH_MIGENTRY_UNLOCK]	= "migentry_unlock",
	[PP_OH_ROUTER_DECISION]	= "router_decision",
};

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
	return 0;
}

static int pp_oh_open(struct inode *inode, struct file *file)
{
	return single_open(file, pp_oh_show, NULL);
}

/* any write resets all counters */
static ssize_t pp_oh_write(struct file *file, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	int i;

	for (i = 0; i < PP_OH_NR_SITES; i++) {
		atomic64_set(&pp_oh_sum_ns[i], 0);
		atomic64_set(&pp_oh_cnt[i], 0);
	}
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
	return 0;
}
late_initcall(pp_oh_init);
