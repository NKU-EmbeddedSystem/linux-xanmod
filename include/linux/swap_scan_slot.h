/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_SCAN_SLOT_H
#define _LINUX_SWAP_SCAN_SLOT_H

#include <linux/swap.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

/* 
 * SWAP_SCAN_SLOT_SIZE
 * migrate target batch size. When scanning batch is full, 
 * stops scanning and start migration process
 */
#define SWAP_SCAN_SLOT_SIZE			  SWAP_BATCH	

/* 
 * SWAP_SLOTS_SCAN_MIN
 * Minimum slot scan number at one attempt.
 * If scanning batch hasn't fill up, scan will
 * be triggered again at next kswapd awake.
 */
#define SWAP_SLOTS_SCAN_MIN            (SWAP_SCAN_SLOT_SIZE * 16)

/*
 * SWAP_SLOTS_SCAN_SAVE_ONCE
 * When swap_vma_readahead is triggerd, 
 * At most this much swap migration process will
 * be triggerd at once. 
 */
#define SWAP_SLOTS_SCAN_SAVE_ONCE		16 


/*
 * SEQ_DIFF_THRESHOLD
 * When swap_vma_readahead is triggerd, 
 * At most this much swap migration process will
 * be triggerd at once. 
 */
#define SEQ_DIFF_THRESHOLD             2

/*
 * Swap scanning watermark thresholds
 *
 * When CONFIG_LRU_GEN_SWAP_ROUTER is enabled, migration uses dynamic thresholds:
 *   - Activate:   utilization >= stress_threshold_very_high (default: 99%)
 *   - Deactivate: utilization < stress_threshold_high (default: 95%)
 *
 * When CONFIG_LRU_GEN_SWAP_ROUTER is disabled, fallback to static watermarks:
 *   - Activate:   free space < 1/32 (>96.875% full)
 *   - Deactivate: free space > 1/16 (<93.75% full)
 */
#define THRESHOLD_ACTIVATE_SWAP_SCAN_SLOT  32
#define THRESHOLD_DEACTIVATE_SWAP_SCAN_SLOT 16

struct swap_scan_slot {
	bool		lock_initialized;
	spinlock_t	scan_lock; /* protects slots, nr, cur */
	bool	scan_stop; /* protects slots, nr, cur */
	int		nr;
	int 	cur;
    struct swap_info_struct * si;
	swp_entry_t	*slots; //store scanned
};

void enable_swap_scan_slot(void);
void disable_swap_scan_slot(void);
swp_entry_t get_next_saved_entry(bool* finished);
void putback_last_saved_entry(swp_entry_t last);
void reenable_scan_cpu(void);

#endif /* _LINUX_SWAP_SLOTS_H */
