/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * Sparse five-level paging for the 57-bit virtual address space.
 */

#include "mm64.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MM64)

#define PT_ENTRIES 512
#define PT_LEVELS 5

static pthread_mutex_t process_table_lock = PTHREAD_MUTEX_INITIALIZER;

struct pt_node {
	union {
		struct pt_node *child[PT_ENTRIES];
		addr_t pte[PT_ENTRIES];
	} entry;
};

static addr_t *lookup_pte(struct mm_struct *mm, addr_t pgn, int create)
{
	addr_t indices[PT_LEVELS];
	struct pt_node *node;
	int level;

	if (mm == NULL || pgn >= (BIT_ULL(57) >> PAGING64_ADDR_PT_SHIFT)) {
		return NULL;
	}
	if (get_pd_from_pagenum(pgn, &indices[0], &indices[1],
	                        &indices[2], &indices[3], &indices[4]) != 0) {
		return NULL;
	}
	if (mm->page_root == NULL) {
		if (!create) {
			return NULL;
		}
		mm->page_root = calloc(1, sizeof(*mm->page_root));
		if (mm->page_root == NULL) {
			return NULL;
		}
		mm->page_table_bytes += sizeof(*mm->page_root);
	}

	node = mm->page_root;
	for (level = 0; level < PT_LEVELS - 1; ++level) {
		struct pt_node **child = &node->entry.child[indices[level]];
		if (*child == NULL) {
			if (!create) {
				return NULL;
			}
			*child = calloc(1, sizeof(**child));
			if (*child == NULL) {
				return NULL;
			}
			mm->page_table_bytes += sizeof(**child);
		}
		node = *child;
	}
	return &node->entry.pte[indices[PT_LEVELS - 1]];
}

static void destroy_node(struct pt_node *node, int level,
                         struct memphy_struct *mram)
{
	int i;
	if (node == NULL) {
		return;
	}
	if (level == PT_LEVELS - 1) {
		for (i = 0; i < PT_ENTRIES; ++i) {
			if (PAGING_PAGE_PRESENT(node->entry.pte[i]) &&
			    mram != NULL) {
				MEMPHY_put_freefp(mram, PAGING_FPN(node->entry.pte[i]));
			}
		}
	} else {
		for (i = 0; i < PT_ENTRIES; ++i) {
			destroy_node(node->entry.child[i], level + 1, mram);
		}
	}
	free(node);
}

static void print_node(struct pt_node *node, int level, addr_t prefix)
{
	int i;
	if (node == NULL) {
		return;
	}
	for (i = 0; i < PT_ENTRIES; ++i) {
		addr_t next = prefix | ((addr_t)i << (PAGING64_ADDR_PT_SHIFT +
		                         (PT_LEVELS - level - 1) * 9));
		if (level == PT_LEVELS - 1) {
			addr_t pte = node->entry.pte[i];
			if (PAGING_PAGE_PRESENT(pte) &&
			    !(pte & PAGING_PTE_SWAPPED_MASK)) {
				printf("  VA 0x%016" PRIx64 " -> frame 0x%" PRIx64 "\n",
				       (uint64_t)next,
				       (uint64_t)PAGING_FPN(pte));
			}
		} else if (node->entry.child[i] != NULL) {
			print_node(node->entry.child[i], level + 1, next);
		}
	}
}

int init_pte(addr_t *pte, int pre, addr_t fpn, int drt,
             int swp, int swptyp, addr_t swpoff)
{
	if (pte == NULL) {
		return -1;
	}
	*pte = 0;
	if (!pre) {
		return 0;
	}
	if (swp) {
		SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
		SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK,
		       PAGING_PTE_SWPTYP_LOBIT);
		SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK,
		       PAGING_PTE_SWPOFF_LOBIT);
	} else {
		SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
		SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK,
		       PAGING_PTE_FPN_LOBIT);
	}
	if (drt) {
		SETBIT(*pte, PAGING_PTE_DIRTY_MASK);
	}
	return 0;
}

int get_pd_from_address(addr_t addr, addr_t *pgd, addr_t *p4d,
                        addr_t *pud, addr_t *pmd, addr_t *pt)
{
	if (pgd == NULL || p4d == NULL || pud == NULL ||
	    pmd == NULL || pt == NULL) {
		return -1;
	}
	*pgd = PAGING64_ADDR_PGD(addr);
	*p4d = PAGING64_ADDR_P4D(addr);
	*pud = PAGING64_ADDR_PUD(addr);
	*pmd = PAGING64_ADDR_PMD(addr);
	*pt = PAGING64_ADDR_PT(addr);
	return 0;
}

int get_pd_from_pagenum(addr_t pgn, addr_t *pgd, addr_t *p4d,
                        addr_t *pud, addr_t *pmd, addr_t *pt)
{
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
	                           pgd, p4d, pud, pmd, pt);
}

addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
	addr_t *pte;
	(void)caller;
	pte = lookup_pte(mm, pgn, 0);
	return pte == NULL ? 0 : *pte;
}

int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t value,
                  struct pcb_t *caller)
{
	addr_t *pte;
	(void)caller;
	pte = lookup_pte(mm, pgn, 1);
	if (pte == NULL) {
		return -1;
	}
	*pte = value;
	return 0;
}

int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn,
                struct pcb_t *caller)
{
	addr_t pte = 0;
	(void)caller;
	if (init_pte(&pte, 1, fpn, 0, 0, 0, 0) != 0) {
		return -1;
	}
	return pte_set_entry(mm, pgn, pte, caller);
}

int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp,
                 addr_t swpoff)
{
	struct mm_struct *mm;
	addr_t pte = 0;
	if (caller == NULL || caller->krnl == NULL) {
		return -1;
	}
	mm = mm_get_process(caller->krnl, caller->pid, 0);
	if (mm == NULL || init_pte(&pte, 1, 0, 0, 1, swptyp, swpoff) != 0) {
		return -1;
	}
	return pte_set_entry(mm, pgn, pte, caller);
}

struct mm_struct *mm_get_process(struct krnl_t *krnl, uint32_t pid,
                                 int kernel_space)
{
	struct process_mm_t *entry;
	struct mm_struct *result = NULL;
	if (krnl == NULL) {
		return NULL;
	}
	pthread_mutex_lock(&process_table_lock);
	for (entry = krnl->process_mms; entry != NULL; entry = entry->next) {
		if (entry->pid == pid) {
			result = kernel_space ? entry->kernel_mm : entry->user_mm;
			break;
		}
	}
	if (entry == NULL) {
		result = kernel_space ? krnl->mm : NULL;
	}
	pthread_mutex_unlock(&process_table_lock);
	return result;
}

int mm_register_process(struct krnl_t *krnl, uint32_t pid,
                        struct mm_struct *user_mm,
                        struct mm_struct *kernel_mm)
{
	struct process_mm_t *entry;
	if (krnl == NULL || user_mm == NULL || kernel_mm == NULL) {
		return -1;
	}
	pthread_mutex_lock(&process_table_lock);
	for (entry = krnl->process_mms; entry != NULL; entry = entry->next) {
		if (entry->pid == pid) {
			pthread_mutex_unlock(&process_table_lock);
			return -1;
		}
	}
	entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		pthread_mutex_unlock(&process_table_lock);
		return -1;
	}
	entry->pid = pid;
	entry->user_mm = user_mm;
	entry->kernel_mm = kernel_mm;
	entry->next = krnl->process_mms;
	krnl->process_mms = entry;
	pthread_mutex_unlock(&process_table_lock);
	return 0;
}

int mm_unregister_process(struct krnl_t *krnl, uint32_t pid)
{
	struct process_mm_t **cursor;
	if (krnl == NULL) {
		return -1;
	}
	pthread_mutex_lock(&process_table_lock);
	for (cursor = &krnl->process_mms; *cursor != NULL;
	     cursor = &(*cursor)->next) {
		if ((*cursor)->pid == pid) {
			struct process_mm_t *entry = *cursor;
			*cursor = entry->next;
			free(entry);
			pthread_mutex_unlock(&process_table_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&process_table_lock);
	return -1;
}

void mm_destroy(struct mm_struct *mm, struct memphy_struct *mram)
{
	struct vm_area_struct *vma;
	struct pgn_t *page;
	if (mm == NULL) {
		return;
	}
	destroy_node(mm->page_root, 0, mram);
	mm->page_root = NULL;
	while ((vma = mm->mmap) != NULL) {
		struct vm_rg_struct *region;
		mm->mmap = vma->vm_next;
		while ((region = vma->vm_freerg_list) != NULL) {
			vma->vm_freerg_list = region->rg_next;
			free(region);
		}
		free(vma);
	}
	while ((page = mm->fifo_pgn) != NULL) {
		mm->fifo_pgn = page->pg_next;
		free(page);
	}
	free(mm->kcpooltbl);
	free(mm);
}

void mm_print_stats(struct mm_struct *mm, uint32_t pid, const char *space)
{
	if (mm != NULL) {
		printf("MM stats pid=%u space=%s: accesses=%" PRIu64
		       " page-walks=%" PRIu64 " page-table-bytes=%" PRIu64 "\n",
		       pid, space, mm->memory_accesses, mm->page_walks,
		       mm->page_table_bytes);
	}
}

static int translate(struct mm_struct *mm, struct krnl_t *krnl,
                     addr_t address, addr_t *physical)
{
	addr_t *pte;
	addr_t pgn = address >> PAGING64_ADDR_PT_SHIFT;
	addr_t offset = address & (PAGING64_PAGESZ - 1);
	if (mm == NULL || krnl == NULL || physical == NULL) {
		return -1;
	}
	++mm->page_walks;
	mm->memory_accesses += PT_LEVELS;
	pte = lookup_pte(mm, pgn, 0);
	if (pte == NULL || !PAGING_PAGE_PRESENT(*pte) ||
	    (*pte & PAGING_PTE_SWAPPED_MASK)) {
		return -1;
	}
	*physical = PAGING_FPN(*pte) * PAGING64_PAGESZ + offset;
	return 0;
}

int mm_read_byte(struct mm_struct *mm, struct krnl_t *krnl,
                 addr_t address, BYTE *value)
{
	addr_t physical;
	if (value == NULL || translate(mm, krnl, address, &physical) != 0 ||
	    krnl->mram == NULL || physical >= (addr_t)krnl->mram->maxsz ||
	    MEMPHY_read(krnl->mram, physical, value) != 0) {
		return -1;
	}
	++mm->memory_accesses;
	return 0;
}

int mm_write_byte(struct mm_struct *mm, struct krnl_t *krnl,
                  addr_t address, BYTE value)
{
	addr_t physical;
	addr_t *pte;
	if (translate(mm, krnl, address, &physical) != 0 ||
	    krnl->mram == NULL || physical >= (addr_t)krnl->mram->maxsz ||
	    MEMPHY_write(krnl->mram, physical, value) != 0) {
		return -1;
	}
	pte = lookup_pte(mm, address >> PAGING64_ADDR_PT_SHIFT, 0);
	if (pte != NULL) {
		SETBIT(*pte, PAGING_PTE_DIRTY_MASK);
	}
	++mm->memory_accesses;
	return 0;
}

int mm_unmap_range(struct mm_struct *mm, struct krnl_t *krnl,
                   addr_t address, addr_t length)
{
	addr_t first;
	addr_t pages;
	addr_t i;
	if (mm == NULL || krnl == NULL || length == 0) {
		return -1;
	}
	first = address / PAGING64_PAGESZ;
	pages = (length + (address % PAGING64_PAGESZ) +
	         PAGING64_PAGESZ - 1) / PAGING64_PAGESZ;
	for (i = 0; i < pages; ++i) {
		addr_t *pte = lookup_pte(mm, first + i, 0);
		if (pte != NULL && PAGING_PAGE_PRESENT(*pte) &&
		    !(*pte & PAGING_PTE_SWAPPED_MASK)) {
			MEMPHY_put_freefp(krnl->mram, PAGING_FPN(*pte));
			*pte = 0;
		}
	}
	return 0;
}

static int alloc_frames(struct krnl_t *krnl, int count,
                        struct framephy_struct **frames)
{
	struct framephy_struct **tail;
	int i;
	if (krnl == NULL || krnl->mram == NULL ||
	    frames == NULL || count <= 0) {
		return -1;
	}
	*frames = NULL;
	tail = frames;
	for (i = 0; i < count; ++i) {
		struct framephy_struct *frame = malloc(sizeof(*frame));
		if (frame == NULL ||
		    MEMPHY_get_freefp(krnl->mram,
		                      frame == NULL ? NULL : &frame->fpn) != 0) {
			free(frame);
			while (*frames != NULL) {
				frame = *frames;
				*frames = frame->fp_next;
				MEMPHY_put_freefp(krnl->mram, frame->fpn);
				free(frame);
			}
			return -3000;
		}
		frame->fp_next = NULL;
		frame->owner = NULL;
		memset(krnl->mram->storage +
		       frame->fpn * PAGING64_PAGESZ, 0, PAGING64_PAGESZ);
		*tail = frame;
		tail = &frame->fp_next;
	}
	return 0;
}

int mm_map_process_range(struct mm_struct *mm, struct krnl_t *krnl,
                         addr_t address, int count)
{
	struct framephy_struct *frames = NULL;
	struct framephy_struct *frame;
	int i;
	int mapped = 0;
	if (mm == NULL || krnl == NULL || krnl->mram == NULL ||
	    address >= BIT_ULL(57) ||
	    address % PAGING64_PAGESZ != 0 || count <= 0 ||
	    (addr_t)count > (BIT_ULL(57) - address) / PAGING64_PAGESZ ||
	    alloc_frames(krnl, count, &frames) != 0) {
		return -1;
	}
	for (i = 0; i < count; ++i) {
		if (pte_get_entry(mm, (address >> PAGING64_ADDR_PT_SHIFT) + i,
		                  NULL) != 0) {
			while (frames != NULL) {
				struct framephy_struct *next = frames->fp_next;
				MEMPHY_put_freefp(krnl->mram, frames->fpn);
				free(frames);
				frames = next;
			}
			return -1;
		}
	}
	frame = frames;
	for (i = 0; i < count; ++i, frame = frame->fp_next) {
		if (pte_set_fpn(mm, (address >> PAGING64_ADDR_PT_SHIFT) + i,
		                frame->fpn, NULL) != 0) {
			int index = 0;
			mm_unmap_range(mm, krnl, address,
			               (addr_t)mapped * PAGING64_PAGESZ);
			for (frame = frames; frame != NULL; ) {
				struct framephy_struct *next = frame->fp_next;
				if (index >= mapped) {
					MEMPHY_put_freefp(krnl->mram, frame->fpn);
				}
				free(frame);
				frame = next;
				++index;
			}
			return -1;
		}
		++mapped;
	}
	for (frame = frames; frame != NULL; ) {
		struct framephy_struct *next = frame->fp_next;
		free(frame);
		frame = next;
	}
	return 0;
}

int mm_map_range(struct mm_struct *mm, struct pcb_t *caller,
                 addr_t address, int count)
{
	return caller == NULL ? -1 :
	       mm_map_process_range(mm, caller->krnl, address, count);
}

addr_t alloc_pages_range(struct pcb_t *caller, int count,
                         struct framephy_struct **frames)
{
	if (caller == NULL) {
		return -1;
	}
	return alloc_frames(caller->krnl, count, frames) == 0 ? 0 : -3000;
}

addr_t vmap_page_range(struct pcb_t *caller, addr_t address, int count,
                       struct framephy_struct *frames,
                       struct vm_rg_struct *region)
{
	struct mm_struct *mm;
	struct framephy_struct *frame = frames;
	int i;
	int mapped = 0;
	if (caller == NULL || caller->krnl == NULL || count <= 0 ||
	    address >= BIT_ULL(57) || address % PAGING64_PAGESZ != 0 ||
	    (count > 0 && frames == NULL)) {
		return -1;
	}
	mm = mm_get_process(caller->krnl, caller->pid, 0);
	if (mm == NULL) {
		return -1;
	}
	for (i = 0; i < count; ++i, frame = frame->fp_next) {
		if (frame == NULL ||
		    pte_get_entry(mm, (address >> PAGING64_ADDR_PT_SHIFT) + i,
		                  caller) != 0 ||
		    pte_set_fpn(mm, (address >> PAGING64_ADDR_PT_SHIFT) + i,
		                frame->fpn, caller) != 0) {
			if (mapped > 0) {
				mm_unmap_range(mm, caller->krnl, address,
				               (addr_t)mapped * PAGING64_PAGESZ);
			}
			return -1;
		}
		++mapped;
	}
	if (region != NULL) {
		region->rg_start = address;
		region->rg_end = address + (addr_t)count * PAGING64_PAGESZ;
	}
	return 0;
}

int vmap_pgd_memset(struct pcb_t *caller, addr_t address, int count)
{
	if (count <= 0 || address % PAGING64_PAGESZ != 0) {
		return -1;
	}
	return caller == NULL || caller->krnl == NULL ? -1 :
	       mm_map_process_range(
	           mm_get_process(caller->krnl, caller->pid, 0),
	           caller->krnl, address, count);
}

addr_t vm_map_ram(struct pcb_t *caller, addr_t start, addr_t end,
                  addr_t mapstart, int count, struct vm_rg_struct *region)
{
	struct mm_struct *mm;
	(void)start;
	(void)end;
	if (count <= 0 || mapstart % PAGING64_PAGESZ != 0) {
		return -1;
	}
	mm = caller == NULL || caller->krnl == NULL ? NULL :
	     mm_get_process(caller->krnl, caller->pid, 0);
	if (mm == NULL || (addr_t)count >
	    (BIT_ULL(57) - mapstart) / PAGING64_PAGESZ) {
		return -1;
	}
	if (mm_map_range(mm, caller, mapstart, count) != 0) {
		return -1;
	}
	if (region != NULL) {
		region->rg_start = mapstart;
		region->rg_end = mapstart + (addr_t)count * PAGING64_PAGESZ;
	}
	return mapstart;
}

int __swap_cp_page(struct memphy_struct *src, addr_t srcfpn,
                   struct memphy_struct *dst, addr_t dstfpn)
{
	addr_t i;
	BYTE value;
	if (src == NULL || dst == NULL || src->storage == NULL ||
	    dst->storage == NULL ||
	    (srcfpn + 1) * PAGING64_PAGESZ > (addr_t)src->maxsz ||
	    (dstfpn + 1) * PAGING64_PAGESZ > (addr_t)dst->maxsz) {
		return -1;
	}
	for (i = 0; i < PAGING64_PAGESZ; ++i) {
		if (MEMPHY_read(src, srcfpn * PAGING64_PAGESZ + i, &value) != 0 ||
		    MEMPHY_write(dst, dstfpn * PAGING64_PAGESZ + i, value) != 0) {
			return -1;
		}
	}
	return 0;
}

int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
	struct vm_area_struct *vma;
	(void)caller;
	if (mm == NULL) {
		return -1;
	}
	memset(mm, 0, sizeof(*mm));
	mm->page_root = calloc(1, sizeof(*mm->page_root));
	vma = calloc(1, sizeof(*vma));
	if (mm->page_root == NULL || vma == NULL) {
		free(mm->page_root);
		free(vma);
		mm->page_root = NULL;
		return -1;
	}
	mm->page_table_bytes = sizeof(*mm->page_root);
	vma->vm_id = 0;
	vma->vm_start = 0;
	vma->vm_end = 0;
	vma->sbrk = 0;
	vma->vm_mm = mm;
	mm->mmap = vma;
	return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t start, addr_t end)
{
	struct vm_rg_struct *region = calloc(1, sizeof(*region));
	if (region != NULL) {
		region->rg_start = start;
		region->rg_end = end;
	}
	return region;
}

int enlist_vm_rg_node(struct vm_rg_struct **list, struct vm_rg_struct *node)
{
	if (list == NULL || node == NULL) {
		return -1;
	}
	node->rg_next = *list;
	*list = node;
	return 0;
}

int enlist_pgn_node(struct pgn_t **list, addr_t pgn)
{
	struct pgn_t *node;
	if (list == NULL) {
		return -1;
	}
	node = malloc(sizeof(*node));
	if (node == NULL) {
		return -1;
	}
	node->pgn = pgn;
	node->pg_next = *list;
	*list = node;
	return 0;
}

int print_list_fp(struct framephy_struct *list)
{
	for (; list != NULL; list = list->fp_next) {
		printf("frame[%" PRIu64 "]\n", (uint64_t)list->fpn);
	}
	return 0;
}

int print_list_rg(struct vm_rg_struct *list)
{
	for (; list != NULL; list = list->rg_next) {
		printf("region[0x%" PRIx64 "->0x%" PRIx64 "]\n",
		       (uint64_t)list->rg_start, (uint64_t)list->rg_end);
	}
	return 0;
}

int print_list_vma(struct vm_area_struct *list)
{
	for (; list != NULL; list = list->vm_next) {
		printf("vma[0x%" PRIx64 "->0x%" PRIx64 "]\n",
		       (uint64_t)list->vm_start, (uint64_t)list->vm_end);
	}
	return 0;
}

int print_list_pgn(struct pgn_t *list)
{
	for (; list != NULL; list = list->pg_next) {
		printf("page[0x%" PRIx64 "]\n", (uint64_t)list->pgn);
	}
	return 0;
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
	struct mm_struct *mm;
	(void)start;
	(void)end;
	if (caller == NULL || caller->krnl == NULL) {
		return -1;
	}
	mm = mm_get_process(caller->krnl, caller->pid, 0);
	if (mm == NULL) {
		return -1;
	}
	printf("Page table pid=%u:\n", caller->pid);
	print_node(mm->page_root, 0, 0);
	mm_print_stats(mm, caller->pid, "user");
	return 0;
}

#endif
