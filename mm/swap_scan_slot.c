#include <linux/swap_scan_slot.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <trace/events/lru_gen.h>
#include "swap.h"
// static DEFINE_PER_CPU(struct swap_scan_slot, swp_scan_slots);
static DEFINE_MUTEX(swap_scan_slot_mutex);
static DEFINE_MUTEX(swap_scan_slot_enable_mutex);
static bool	swap_scan_slot_initialized;
extern unsigned int swap_scan_savior_enabled; /* vmscan.c debugfs migrator toggle */
static bool swap_scan_slot_enabled;
static bool	swap_scan_slot_active;
extern bool swap_scan_enabled_sysfs;
#define use_swap_scan_slot (swap_scan_slot_enabled && swap_scan_slot_active && swap_scan_enabled_sysfs)

struct swap_scan_slot global_swp_scan_slot;

static void deactivate_swap_scan_slot(void)
{
	if (!swap_scan_slot_active)
		return;
	mutex_lock(&swap_scan_slot_mutex);
	swap_scan_slot_active = false;
	// pr_err("deactivate_swap_scan_slot");
	mutex_unlock(&swap_scan_slot_mutex);
}

static void reactivate_swap_scan_slot(void)
{
	if (swap_scan_slot_active)
		return;
	mutex_lock(&swap_scan_slot_mutex);
	swap_scan_slot_active = true;
	// pr_err("reactivate_swap_scan_slot");
	mutex_unlock(&swap_scan_slot_mutex);
}

void check_swap_scan_active(struct swap_info_struct *si, long left, long total)
{
	unsigned int fill_permille;

	if (!swap_scan_slot_enabled)
		return;
	if (!__si_can_version(si))
		return;

#ifdef CONFIG_LRU_GEN_SWAP_ROUTER
	/*
	 * Use dynamic thresholds aligned with router auto-adjustment system.
	 * Migration starts at critical utilization (≥99%) and stops when
	 * utilization drops below high threshold (<95%).
	 *
	 * This creates hysteresis to prevent oscillation and aligns migration
	 * behavior with the stress-based parameter adjustment.
	 */
	extern unsigned int stress_threshold_very_high;	/* Default: 990 (99%) */
	extern unsigned int stress_threshold_high;		/* Default: 950 (95%) */

	if (unlikely(total == 0))
		return;

	/* Calculate utilization in permille: (used / total) * 1000 */
	fill_permille = ((total - left) * 1000) / total;

	/* Activate migration when utilization reaches critical level (≥99%) */
	if (fill_permille >= READ_ONCE(stress_threshold_very_high)) {
		reactivate_swap_scan_slot();
		trace_swap_scan_change_state(1, left, total);
	}

	/* Deactivate migration when utilization drops below high threshold (<95%) */
	if (fill_permille < READ_ONCE(stress_threshold_high)) {
		deactivate_swap_scan_slot();
		trace_swap_scan_change_state(0, left, total);
	}
#else
	/* Fall back to original hardcoded watermarks if router not enabled */
	if (left * THRESHOLD_ACTIVATE_SWAP_SCAN_SLOT < total) {
		reactivate_swap_scan_slot();
		trace_swap_scan_change_state(1, left, total);
	}

	if (left * THRESHOLD_DEACTIVATE_SWAP_SCAN_SLOT > total) {
		deactivate_swap_scan_slot();
		trace_swap_scan_change_state(0, left, total);
	}
#endif /* CONFIG_LRU_GEN_SWAP_ROUTER */
}

static int alloc_swap_scan_slot(unsigned int cpu)
{
	struct swap_scan_slot *cache;
    swp_entry_t *slots;

	slots = kvcalloc(SWAP_SCAN_SLOT_SIZE, sizeof(swp_entry_t),
			 GFP_KERNEL);
	if (!slots){
		return -ENOMEM;
	}
	cache = &global_swp_scan_slot;
	if (cache->slots){
		kvfree(slots);
		pr_err("already alloced ?");
		return 0;
	}
	if (!cache->lock_initialized) {
		spin_lock_init(&cache->scan_lock);
		cache->lock_initialized = true;
	}
	cache->nr = 0;
	cache->cur = 0;
	cache->scan_stop = false;
	mb();
	cache->slots = slots;
    return 0;
}

// static void drain_swap_scan_slot_cpu(unsigned int cpu, bool free_slots)
// {
// 	struct swap_scan_slot *cache;
	
//     cache = &per_cpu(swp_scan_slots, cpu);
// 	spin_lock_irq(&cache->scan_lock);

//     cache->nr = 0;
// 	if (free_slots && cache->slots) {
//         kvfree(cache->slots);
//         cache->slots = NULL;
//     }
//     spin_unlock_irq(&cache->scan_lock);
// }

static void __drain_swap_scan_slot(void){
	// unsigned int cpu;
	// for_each_online_cpu(cpu)
	// 	drain_swap_scan_slot_cpu(cpu, false);
	struct swap_scan_slot *cache;
	cache = &global_swp_scan_slot;
	spin_lock_irq(&cache->scan_lock);
	cache->nr = 0;
    spin_unlock_irq(&cache->scan_lock);
}

static int free_swap_scan_slot(unsigned int cpu)
{
	mutex_lock(&swap_scan_slot_mutex);
    // drain_swap_scan_slot_cpu(cpu, true);
	__drain_swap_scan_slot();
    mutex_unlock(&swap_scan_slot_mutex);
	return 0;
}

static void __reenable_swap_scan_slot(void)
{
	swap_scan_slot_enabled = has_usable_swap();
	if (!swap_scan_slot_enabled)
		pr_err("there's no usable swap enable[%d]", swap_scan_slot_enabled);
}

void disable_swap_scan_slot_lock(void)
{
	mutex_lock(&swap_scan_slot_enable_mutex);
	swap_scan_slot_enabled = false;
	if (swap_scan_slot_initialized) {
		cpus_read_lock();
		__drain_swap_scan_slot();
		cpus_read_unlock();
	}
}

void reenable_swap_scan_slot_unlock(void){
	__reenable_swap_scan_slot();
	mutex_unlock(&swap_scan_slot_enable_mutex);
}

void reenable_scan_cpu(void){
	struct swap_scan_slot *cache;
	// cache = raw_cpu_ptr(&swp_scan_slots);
	cache = &global_swp_scan_slot;
	__drain_swap_scan_slot();
	spin_lock_irq(&cache->scan_lock);

	cache->scan_stop = false;
	spin_unlock_irq(&cache->scan_lock);
}

//might fail
void putback_last_saved_entry(swp_entry_t last){
	struct swap_scan_slot *cache;
	cache = &global_swp_scan_slot;//raw_cpu_ptr(&swp_scan_slots);
	spin_lock_irq(&cache->scan_lock); //put it in the last space
	if (cache->cur == 0)
		return;
	cache->cur--;
	cache->slots[cache->cur] = last;
	spin_unlock_irq(&cache->scan_lock);
}


/* 
 * get_next_saved_entry 
 * return one target from scan batch, 
 * informs caller weather this batch has finished, 
 * When the last victim has been consumed, re-
 * activate migration swap scan 
 * - paramter finished has to be valid 
 */
#define INVALID_SWP_ENTRY swp_entry(MAX_SWAPFILES, 0) 
swp_entry_t get_next_saved_entry(bool* finished){
	struct swap_scan_slot *cache;
	swp_entry_t entry;
	entry = INVALID_SWP_ENTRY; 
	cache = &global_swp_scan_slot;
	if (unlikely(!cache)) {
		*finished = true;
		return entry;
	}

	spin_lock_irq(&cache->scan_lock);
	/* PagePilot bug#7: the consumer must be gated on the migrator toggle
	 * too -- only the PRODUCER used to check it, so a route-only run
	 * would drain a stale batch left over from a previous migroute run. */
	if (!cache->scan_stop || !use_swap_scan_slot || !cache->slots ||
	    !READ_ONCE(swap_scan_savior_enabled)){
		*finished = true;
		spin_unlock_irq(&cache->scan_lock);
		return entry;
	}
	/* PagePilot bug#7: validate cursor state BEFORE the read. The old
	 * exact-equality termination below never fired once cur overshot nr
	 * (observed cur in the millions -> reads walked off the slot array
	 * into unmapped memory -> oops). Reset on any inconsistency. */
	if (unlikely(cache->cur < 0 || cache->cur >= cache->nr ||
		     cache->nr > SWAP_SCAN_SLOT_SIZE)){
		WARN_ONCE(1, "scan slot state corrupt (cur[%d] nr[%d]), resetting",
			  cache->cur, cache->nr);
		cache->cur = 0;
		cache->nr = 0;
		cache->scan_stop = false;
		*finished = true;
		spin_unlock_irq(&cache->scan_lock);
		return entry;
	}
	//we start read
	entry = cache->slots[cache->cur];
	cache->cur++;
	*finished = false;
	if (unlikely(cache->cur >= cache->nr)){
		cache->cur = 0;
		cache->nr = 0;
		cache->scan_stop = false;
		*finished = true;
	}
	spin_unlock_irq(&cache->scan_lock);
	VM_BUG_ON(non_swap_entry(entry));

	return entry;
}

/*
 * PagePilot bug#7: drop any half-consumed scan batch. Called when the
 * migrator toggle flips so stale queue state never survives a mode
 * switch (a route-only run must find an empty queue, not a leftover
 * batch from the previous migroute run).
 */
void reset_swap_scan_slot(void)
{
	struct swap_scan_slot *cache = &global_swp_scan_slot;
	unsigned long flags;

	if (!swap_scan_slot_initialized || !cache->lock_initialized)
		return;
	spin_lock_irqsave(&cache->scan_lock, flags);
	cache->cur = 0;
	cache->nr = 0;
	cache->scan_stop = false;
	spin_unlock_irqrestore(&cache->scan_lock, flags);
}

int add_to_scan_slot(swp_entry_t entry)
{
	struct swap_scan_slot *cache;
	int cpu;
	unsigned long flags;

	cache = &global_swp_scan_slot;//raw_cpu_ptr(&swp_scan_slots);
	cpu = smp_processor_id();

	spin_lock_irqsave(&cache->scan_lock, flags);
	if (likely(use_swap_scan_slot && cache->slots && ! cache->scan_stop)){
		// spin_lock_irq(&cache->scan_lock);
		if (!use_swap_scan_slot || !cache->slots){
			if (cache->slots)
				pr_err("add_to_scan_slot stopped by use_swap_scan_slot[%d]enable[%d]sysfs[%d]", 
							use_swap_scan_slot, swap_scan_slot_enabled, swap_scan_enabled_sysfs);
			goto fail_add;
		}
		if (unlikely(non_swap_entry(entry))){
			pr_err_ratelimited("add_to_scan_slot received bad entry [%lx]", entry.val);
			goto fail_add; /* fail_add unlocks; unlocking here too was a double-unlock */
		}
		if (unlikely(swp_entry_test_special(entry) || swp_entry_test_ext(entry))){
			/* queue must only carry plain entries; WARN captures the
			 * producer's stack to locate who feeds malformed values */
			WARN_ONCE(1, "add_to_scan_slot rejecting special/ext entry [%lx]", entry.val);
			goto fail_add;
		}
		cache->slots[cache->nr++] = entry;
		if (cache->nr >= SWAP_SCAN_SLOT_SIZE){ //FULL NOW
			// swap_scan_save_entries(cache->slots, cache->nr);
			trace_add_to_scan_slot(cpu, cache->cur, cache->nr);

			cache->cur = 0; //read from start
			cache->scan_stop = true;
			MULTISWAP_MIG_INFO("scan_slot cache full [%d]", cache->nr);
			//this will block  use_swap_scan_slot
			spin_unlock_irq(&cache->scan_lock);
			return -2;
		}
		spin_unlock_irq(&cache->scan_lock);
		return 0; //success add
	}
fail_add:
	spin_unlock_irqrestore(&cache->scan_lock, flags);
	return -1;
}

void enable_swap_scan_slot(void)
{
	int ret;
	mutex_lock(&swap_scan_slot_enable_mutex);
    if (!swap_scan_slot_initialized){
		pr_err("try initialize swap scan");
		ret = alloc_swap_scan_slot(smp_processor_id());
        // ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "swap_scan_slot", 
        //             alloc_swap_scan_slot, free_swap_scan_slot);
        if (WARN_ONCE(ret < 0, "Cache allocation failed (%s), operating "
				       "without swap scan slot.\n", __func__))
		 	goto out_unlock;
		pr_err("swap scan initialized ok");
        swap_scan_slot_initialized = true;
    }
	mb();
    __reenable_swap_scan_slot();
out_unlock:
	mutex_unlock(&swap_scan_slot_enable_mutex);
	pr_err("swap scan enabled");
}
