#ifndef _MM_MULTISWP_H_
#define _MM_MULTISWP_H_
#ifdef CONFIG_SWAP

#define SWAP_REMAP_ADDRESS_SPACE_SHIFT	(14 + 3)

/* One swap address space rmap for each 512M swap space */
/* 
 * This is copied from swap_cache's address space. 
 * We choosed << 17 instead of  << 14 , because fast swap
 * cache is dense, however,  this one shouldn't be that dense.
 */
#define SWAP_ADDRESS_SPACE_REMAP_SHIFT	(SWAP_REMAP_ADDRESS_SPACE_SHIFT)
#define SWAP_ADDRESS_SPACE_REMAP_PAGES	(1 << SWAP_REMAP_ADDRESS_SPACE_SHIFT)
extern struct address_space *swapper_spaces_remap[];
#define swap_address_space_remap(entry)			    \
	(&swapper_spaces_remap[swp_type(entry)][(((swp_raw_offset(entry) \
		>> SWAP_REMAP_ADDRESS_SPACE_SHIFT) << SWP_SPECIAL_MARK) | (swp_entry_test_special(entry)))])

#define SWAPVMAX (SWP_ENTRY_ALIVE_VERSION)
#define VMAXMASK (((1ULL << ((unsigned long long)SWAPVMAX * 8ULL)) - 1ULL) )
#define VERSION_OFFSET(v, off, vmax) (vmax * off + v)
#define VERSION_OFFSET_SI(v, off, vmax, si) (__si_can_version(si) ?  VERSION_OFFSET(v, off, vmax) : off)

void *get_shadow_from_swap_cache(swp_entry_t entry);
void *get_shadow_from_swap_cache_erase(swp_entry_t entry);
int add_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t to_entry, 
    gfp_t gfp);
int enable_swp_entry_remap(struct folio* folio, swp_entry_t from_entry, swp_entry_t* p_to_entry);
void delete_from_swap_remap(struct folio *folio, swp_entry_t entry_from, swp_entry_t entry_to, bool delete_unpepared);
void delete_from_swap_remap_get_mig(struct folio* folio, swp_entry_t entry_from, swp_entry_t* entry_to);
void delete_from_swap_remap_raw(swp_entry_t entry_from, swp_entry_t entry_to);
void __delete_from_swap_cache_mig(struct folio *folio,
    swp_entry_t entry, bool shadow_transfer);
void swap_remap_unlock(struct folio *folio, swp_entry_t ori_swap, swp_entry_t mig_swap);
void clear_swap_remap_entire(struct swap_info_struct *si);
swp_entry_t folio_get_migentry(struct folio* folio, swp_entry_t ori);
int entry_remap_usable_version(swp_entry_t entry);
swp_entry_t entry_get_migentry(swp_entry_t ori_swap);
swp_entry_t entry_get_migentry_lock(swp_entry_t ori_swap);
swp_entry_t entry_get_migentry_unlock(swp_entry_t ori_swap, swp_entry_t _mig_swap);
void delete_from_swap_cache_mig(struct folio* folio, swp_entry_t entry, bool sub_ref, bool transfer_shadow);
void clear_shadow_from_swap_cache(int type, unsigned long begin,
    unsigned long end, int free);
void swap_shadow_scan_next(struct swap_info_struct * si, struct lruvec * lruvec, 
    unsigned long* scanned, unsigned long* saved);
int __si_can_version(struct swap_info_struct *si);

#endif /* CONFIG_SWAP */
#endif /* _MM_MULTISWP_H_ */