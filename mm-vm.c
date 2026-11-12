/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * Virtual memory area management for the separated user address spaces.
 */

#include "mm64.h"
#include <limits.h>
#include <stdlib.h>

struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
	struct vm_area_struct *vma;
	if (mm == NULL || vmaid < 0) {
		return NULL;
	}
	for (vma = mm->mmap; vma != NULL; vma = vma->vm_next) {
		if (vma->vm_id == (unsigned long)vmaid) {
			return vma;
		}
	}
	return NULL;
}

int __mm_swap_page(struct pcb_t *caller, addr_t victim_fpn,
                   addr_t swap_fpn)
{
	if (caller == NULL || caller->krnl == NULL ||
	    caller->krnl->mram == NULL || caller->krnl->active_mswp == NULL) {
		return -1;
	}
	return __swap_cp_page(caller->krnl->mram, victim_fpn,
	                      caller->krnl->active_mswp, swap_fpn);
}

struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller,
                                             int vmaid, addr_t size)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct vm_rg_struct *region;
	if (caller == NULL || caller->krnl == NULL) {
		return NULL;
	}
	mm = mm_get_process(caller->krnl, caller->pid, 0);
	vma = get_vma_by_num(mm, vmaid);
	if (vma == NULL || size == 0 || UINT64_MAX - vma->sbrk < size) {
		return NULL;
	}
	region = init_vm_rg(vma->sbrk, vma->sbrk + size);
	if (region != NULL) {
		region->vmaid = vmaid;
	}
	return region;
}

int validate_overlap_vm_area(struct pcb_t *caller, int vmaid,
                             addr_t start, addr_t end)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct vm_area_struct *target;
	if (caller == NULL || caller->krnl == NULL || start >= end) {
		return -1;
	}
	mm = mm_get_process(caller->krnl, caller->pid, 0);
	target = get_vma_by_num(mm, vmaid);
	if (mm == NULL || target == NULL) {
		return -1;
	}
	for (vma = mm->mmap; vma != NULL; vma = vma->vm_next) {
		if (vma != target && start < vma->vm_end && vma->vm_start < end) {
			return -1;
		}
	}
	return 0;
}

int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t increment)
{
	if (caller == NULL) {
		return -1;
	}
	return mm_expand_process(caller->krnl, caller->pid, vmaid, increment);
}

int mm_expand_process(struct krnl_t *krnl, uint32_t pid,
                      int vmaid, addr_t increment)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	addr_t pages;
	addr_t mapped_size;
	if (krnl == NULL || increment == 0) {
		return -1;
	}
	mm = mm_get_process(krnl, pid, 0);
	vma = get_vma_by_num(mm, vmaid);
	if (mm == NULL || vma == NULL ||
	    increment > UINT64_MAX - (PAGING64_PAGESZ - 1)) {
		return -1;
	}
	pages = (increment + PAGING64_PAGESZ - 1) / PAGING64_PAGESZ;
	mapped_size = pages * PAGING64_PAGESZ;
	if (pages > INT_MAX || mapped_size > UINT64_MAX - vma->sbrk ||
	    vma->sbrk + mapped_size >= BIT_ULL(57)) {
		return -1;
	}
	if (mm_map_process_range(mm, krnl, vma->sbrk, (int)pages) != 0) {
		return -1;
	}
	vma->sbrk += mapped_size;
	vma->vm_end = vma->sbrk;
	return 0;
}
