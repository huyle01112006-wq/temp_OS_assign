/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * Kernel-side memory system calls. A process PCB is never constructed or
 * dereferenced here; its address space is found by PID through krnl_t.
 */

#include "os-mm.h"
#include "syscall.h"
#include "mm.h"
#include <stdio.h>
#include <limits.h>

#define SYSMEM_MAP_OP 1
#define SYSMEM_INC_OP 2
#define SYSMEM_SWP_OP 3
#define SYSMEM_IO_READ 4
#define SYSMEM_IO_WRITE 5

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
	struct mm_struct *user_mm;
	BYTE value;
	if (krnl == NULL || regs == NULL) {
		return -1;
	}
	user_mm = mm_get_process(krnl, pid, 0);
	switch (regs->a1) {
	case SYSMEM_MAP_OP:
		if (user_mm == NULL || regs->a3 == 0 || regs->a3 > INT_MAX) {
			return -1;
		}
		return mm_map_process_range(user_mm, krnl, regs->a2,
		                            (int)regs->a3);
	case SYSMEM_INC_OP:
		if (regs->a2 > INT_MAX) {
			return -1;
		}
		return mm_expand_process(krnl, pid, (int)regs->a2, regs->a3);
	case SYSMEM_SWP_OP:
		if (krnl->mram == NULL || krnl->active_mswp == NULL) {
			return -1;
		}
		return __swap_cp_page(krnl->mram, regs->a2,
		                      krnl->active_mswp, regs->a3);
	case SYSMEM_IO_READ:
		if (krnl->mram == NULL ||
		    MEMPHY_read(krnl->mram, regs->a2, &value) != 0) {
			return -1;
		}
		regs->a3 = (unsigned char)value;
		return 0;
	case SYSMEM_IO_WRITE:
		if (krnl->mram == NULL) {
			return -1;
		}
		value = (BYTE)regs->a3;
		return MEMPHY_write(krnl->mram, regs->a2, value);
	default:
		fprintf(stderr, "Unknown memory operation: %llu\n",
		        (unsigned long long)regs->a1);
		return -1;
	}
}
