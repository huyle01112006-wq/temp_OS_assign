/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * User and kernel memory operations. Process address spaces are resolved
 * through the kernel's PID-to-MM table rather than through PCB MM fields.
 */

#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

static struct mm_struct *process_mm(struct pcb_t *caller, int kernel_space)
{
	if (caller == NULL || caller->krnl == NULL) {
		return NULL;
	}
	return mm_get_process(caller->krnl, caller->pid, kernel_space);
}

int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *region)
{
	if (mm == NULL || mm->mmap == NULL || region == NULL ||
	    region->rg_start >= region->rg_end) {
		return -1;
	}
	region->rg_next = mm->mmap->vm_freerg_list;
	mm->mmap->vm_freerg_list = region;
	return 0;
}

struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
	if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ) {
		return NULL;
	}
	return &mm->symrgtbl[rgid];
}

int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size,
                       struct vm_rg_struct *newrg)
{
	struct mm_struct *mm = process_mm(caller, 0);
	struct vm_area_struct *vma = get_vma_by_num(mm, vmaid);
	struct vm_rg_struct **cursor;
	if (vma == NULL || newrg == NULL || size <= 0) {
		return -1;
	}
	for (cursor = &vma->vm_freerg_list; *cursor != NULL;
	     cursor = &(*cursor)->rg_next) {
		struct vm_rg_struct *region = *cursor;
		if (region->rg_end - region->rg_start >= (addr_t)size) {
			newrg->rg_start = region->rg_start;
			newrg->rg_end = region->rg_start + (addr_t)size;
			newrg->vmaid = vmaid;
			region->rg_start = newrg->rg_end;
			if (region->rg_start == region->rg_end) {
				*cursor = region->rg_next;
				free(region);
			}
			return 0;
		}
	}
	return -1;
}

static int allocate_region(struct pcb_t *caller, struct mm_struct *mm,
                           int vmaid, int rgid, addr_t size,
                           addr_t *address)
{
	struct vm_area_struct *vma;
	struct vm_rg_struct *symbol;
	addr_t page_size = PAGING64_PAGESZ;
	addr_t mapped_size;
	addr_t pages;
	addr_t start;
	struct vm_rg_struct **free_cursor;
	struct vm_rg_struct *free_region = NULL;
	if (caller == NULL || caller->krnl == NULL || mm == NULL ||
	    address == NULL || size == 0 || rgid < 0 ||
	    rgid >= PAGING_MAX_SYMTBL_SZ ||
	    size > UINT64_MAX - (page_size - 1)) {
		return -1;
	}
	vma = get_vma_by_num(mm, vmaid);
	symbol = get_symrg_byid(mm, rgid);
	if (vma == NULL || symbol == NULL ||
	    symbol->rg_start != 0 || symbol->rg_end != 0) {
		return -1;
	}
	pages = (size + page_size - 1) / page_size;
	mapped_size = pages * page_size;
	start = vma->sbrk;
	for (free_cursor = &vma->vm_freerg_list; *free_cursor != NULL;
	     free_cursor = &(*free_cursor)->rg_next) {
		if ((*free_cursor)->rg_end - (*free_cursor)->rg_start >=
		    mapped_size) {
			free_region = *free_cursor;
			start = free_region->rg_start;
			break;
		}
	}
	if (pages > INT_MAX || mapped_size > UINT64_MAX - start ||
	    start + mapped_size >= BIT_ULL(57) ||
	    mm_map_range(mm, caller, start, (int)pages) != 0) {
		return -1;
	}
	if (free_region != NULL) {
		free_region->rg_start += mapped_size;
		if (free_region->rg_start == free_region->rg_end) {
			*free_cursor = free_region->rg_next;
			free(free_region);
		}
	} else {
		vma->sbrk = start + mapped_size;
		vma->vm_end = vma->sbrk;
	}
	symbol->vmaid = vmaid;
	symbol->rg_start = start;
	symbol->rg_end = start + size;
	*address = start;
	return 0;
}

int __alloc(struct pcb_t *caller, int vmaid, int rgid,
            addr_t size, addr_t *alloc_addr)
{
	int result;
	struct mm_struct *mm = process_mm(caller, 0);
	pthread_mutex_lock(&mmvm_lock);
	result = allocate_region(caller, mm, vmaid, rgid, size, alloc_addr);
	pthread_mutex_unlock(&mmvm_lock);
	return result;
}

int __free(struct pcb_t *caller, int vmaid, int rgid)
{
	struct mm_struct *mm = process_mm(caller, 0);
	struct vm_rg_struct *symbol;
	struct vm_rg_struct *free_region;
	int result = -1;
	if (mm == NULL || caller == NULL || caller->krnl == NULL) {
		return -1;
	}
	pthread_mutex_lock(&mmvm_lock);
	symbol = get_symrg_byid(mm, rgid);
	if (symbol != NULL && (symbol->rg_start != 0 || symbol->rg_end != 0) &&
	    symbol->vmaid == vmaid) {
		addr_t start = symbol->rg_start;
		addr_t length = symbol->rg_end - symbol->rg_start;
		struct vm_area_struct *vma = get_vma_by_num(mm, vmaid);
		free_region = init_vm_rg(start,
		    start + ((length + PAGING64_PAGESZ - 1) /
		             PAGING64_PAGESZ) * PAGING64_PAGESZ);
		if (vma != NULL && free_region != NULL &&
		    mm_unmap_range(mm, caller->krnl, start, length) == 0) {
			free_region->vmaid = vmaid;
			enlist_vm_freerg_list(mm, free_region);
			symbol->rg_start = 0;
			symbol->rg_end = 0;
			result = 0;
		} else {
			free(free_region);
		}
	}
	pthread_mutex_unlock(&mmvm_lock);
	return result;
}

int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
	addr_t address;
	if (reg_index >= sizeof(proc->regs) / sizeof(proc->regs[0]) ||
	    __alloc(proc, 0, (int)reg_index, size, &address) != 0) {
		return -1;
	}
	proc->regs[reg_index] = address;
	return 0;
}

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
	return __free(proc, 0, (int)reg_index);
}

int pg_getpage(struct mm_struct *mm, int pgn, int *fpn,
               struct pcb_t *caller)
{
	addr_t pte;
	(void)caller;
	if (mm == NULL || pgn < 0 || fpn == NULL) {
		return -1;
	}
	pte = pte_get_entry(mm, (addr_t)pgn, caller);
	if (!PAGING_PAGE_PRESENT(pte) || (pte & PAGING_PTE_SWAPPED_MASK)) {
		return -1;
	}
	*fpn = (int)PAGING_FPN(pte);
	return 0;
}

int pg_getval(struct mm_struct *mm, int address, BYTE *data,
              struct pcb_t *caller)
{
	if (caller == NULL || data == NULL) {
		return -1;
	}
	return mm_read_byte(mm, caller->krnl, (addr_t)(uint32_t)address, data);
}

int pg_setval(struct mm_struct *mm, int address, BYTE value,
              struct pcb_t *caller)
{
	if (caller == NULL) {
		return -1;
	}
	return mm_write_byte(mm, caller->krnl,
	                     (addr_t)(uint32_t)address, value);
}

int __read(struct pcb_t *caller, int vmaid, int rgid,
           addr_t offset, BYTE *data)
{
	struct mm_struct *mm = process_mm(caller, 0);
	struct vm_rg_struct *region = get_symrg_byid(mm, rgid);
	int result;
	if (caller == NULL || caller->krnl == NULL || region == NULL ||
	    region->vmaid != vmaid || data == NULL ||
	    offset >= region->rg_end - region->rg_start) {
		return -1;
	}
	pthread_mutex_lock(&mmvm_lock);
	result = mm_read_byte(mm, caller->krnl, region->rg_start + offset, data);
	pthread_mutex_unlock(&mmvm_lock);
	return result;
}

int libread(struct pcb_t *proc, uint32_t source, addr_t offset,
            uint32_t *destination)
{
	BYTE value;
	if (proc == NULL || destination == NULL ||
	    *destination >= sizeof(proc->regs) / sizeof(proc->regs[0]) ||
	    __read(proc, 0, (int)source, offset, &value) != 0) {
		return -1;
	}
	proc->regs[*destination] = (uint8_t)value;
	return 0;
}

int __write(struct pcb_t *caller, int vmaid, int rgid,
            addr_t offset, BYTE value)
{
	struct mm_struct *mm = process_mm(caller, 0);
	struct vm_rg_struct *region = get_symrg_byid(mm, rgid);
	int result;
	if (caller == NULL || caller->krnl == NULL || region == NULL ||
	    region->vmaid != vmaid ||
	    offset >= region->rg_end - region->rg_start) {
		return -1;
	}
	pthread_mutex_lock(&mmvm_lock);
	result = mm_write_byte(mm, caller->krnl, region->rg_start + offset, value);
	pthread_mutex_unlock(&mmvm_lock);
	return result;
}

int libwrite(struct pcb_t *proc, BYTE data, uint32_t destination,
             addr_t offset)
{
	return __write(proc, 0, (int)destination, offset, data);
}

int libkmem_malloc(struct pcb_t *caller, uint32_t size, uint32_t reg_index)
{
	addr_t address;
	struct mm_struct *mm;
	int result;
	if (caller == NULL ||
	    reg_index >= sizeof(caller->regs) / sizeof(caller->regs[0])) {
		return -1;
	}
	mm = process_mm(caller, 1);
	pthread_mutex_lock(&kmm_lock);
	result = allocate_region(caller, mm, 0, (int)reg_index, size, &address);
	pthread_mutex_unlock(&kmm_lock);
	if (result == 0 && reg_index < sizeof(caller->regs) / sizeof(caller->regs[0])) {
		caller->regs[reg_index] = address;
		return 0;
	}
	return -1;
}

addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid,
                 addr_t size, addr_t *alloc_addr)
{
	return allocate_region(caller, process_mm(caller, 1), vmaid, rgid,
	                       size, alloc_addr);
}

int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst,
                      uint32_t offset, uint32_t size)
{
	if (caller == NULL || size == 0) {
		fprintf(stderr, "DMA request rejected: invalid caller or size\n");
		return -1;
	}
	return do_dma(caller, (uint64_t)src + offset,
	              (uint64_t)dst + offset, (uint64_t)size);
}

int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid,
                      addr_t offset, BYTE *data)
{
	struct mm_struct *mm = process_mm(caller, 1);
	struct vm_rg_struct *region = get_symrg_byid(mm, rgid);
	if (caller == NULL || region == NULL || region->vmaid != vmaid ||
	    data == NULL || offset >= region->rg_end - region->rg_start) {
		return -1;
	}
	return mm_read_byte(mm, caller->krnl,
	                    region->rg_start + offset, data);
}

int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid,
                       addr_t offset, BYTE value)
{
	struct mm_struct *mm = process_mm(caller, 1);
	struct vm_rg_struct *region = get_symrg_byid(mm, rgid);
	if (caller == NULL || region == NULL || region->vmaid != vmaid ||
	    offset >= region->rg_end - region->rg_start) {
		return -1;
	}
	return mm_write_byte(mm, caller->krnl,
	                     region->rg_start + offset, value);
}

int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid,
                    addr_t offset, BYTE *data)
{
	return __read(caller, vmaid, rgid, offset, data);
}

int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid,
                     addr_t offset, BYTE value)
{
	return __write(caller, vmaid, rgid, offset, value);
}

int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source,
                           uint32_t destination, uint32_t offset,
                           uint32_t size)
{
	uint32_t i;
	if (size == 0) {
		return -1;
	}
	for (i = 0; i < size; ++i) {
		BYTE value;
		if (__read_user_mem(caller, 0, (int)source,
		                    (addr_t)offset + i, &value) != 0 ||
		    __write_kernel_mem(caller, 0, (int)destination,
		                       (addr_t)offset + i, value) != 0) {
			return -1;
		}
	}
	return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source,
                         uint32_t destination, uint32_t offset,
                         uint32_t size)
{
	uint32_t i;
	if (size == 0) {
		return -1;
	}
	for (i = 0; i < size; ++i) {
		BYTE value;
		if (__read_kernel_mem(caller, 0, (int)source,
		                      (addr_t)offset + i, &value) != 0 ||
		    __write_user_mem(caller, 0, (int)destination,
		                     (addr_t)offset + i, value) != 0) {
			return -1;
		}
	}
	return 0;
}

int free_pcb_memph(struct pcb_t *caller)
{
	struct mm_struct *mm = process_mm(caller, 0);
	struct vm_area_struct *vma;
	if (caller == NULL || caller->krnl == NULL || mm == NULL) {
		return -1;
	}
	for (vma = mm->mmap; vma != NULL; vma = vma->vm_next) {
		if (vma->vm_end > vma->vm_start &&
		    mm_unmap_range(mm, caller->krnl, vma->vm_start,
		                   vma->vm_end - vma->vm_start) != 0) {
			return -1;
		}
	}
	return 0;
}

int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
	struct pgn_t **cursor;
	struct pgn_t *victim;
	if (mm == NULL || retpgn == NULL || mm->fifo_pgn == NULL) {
		return -1;
	}
	for (cursor = &mm->fifo_pgn; (*cursor)->pg_next != NULL;
	     cursor = &(*cursor)->pg_next) {
	}
	victim = *cursor;
	*retpgn = victim->pgn;
	*cursor = NULL;
	free(victim);
	return 0;
}
