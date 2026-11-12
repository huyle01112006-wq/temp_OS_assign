/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Memory physical module mm/mm-memphy.c
 */

#include "mm.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

static pthread_mutex_t free_frame_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   int numstep = 0;

   mp->cursor = 0;
   while (numstep < offset && numstep < mp->maxsz)
   {
      /* Traverse sequentially */
      mp->cursor = (mp->cursor + 1) % mp->maxsz;
      numstep++;
   }

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || mp->storage == NULL || value == NULL ||
       addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      return -1;

   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[mp->cursor];

   return 0;
}

/*
 *  MEMPHY_read read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || mp->storage == NULL || value == NULL ||
       addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      *value = mp->storage[addr];
   else /* Sequential access device */
      return MEMPHY_seq_read(mp, addr, value);

   return 0;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{
   if (mp == NULL || mp->storage == NULL || addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      return -1;

   MEMPHY_mv_csr(mp, addr);
   mp->storage[mp->cursor] = value;

   return 0;
}

/*
 *  MEMPHY_write-write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   if (mp == NULL || mp->storage == NULL || addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      mp->storage[addr] = data;
   else /* Sequential access device */
      return MEMPHY_seq_write(mp, addr, data);

   return 0;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  @mp: memphy struct
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting come with fixed constant PAGESZ */
   int numfp = mp->maxsz / pagesz;
   struct framephy_struct *newfst, *fst;
   int iter = 0;

   if (numfp <= 0)
      return -1;

   /* Init head of free framephy list */
   fst = malloc(sizeof(struct framephy_struct));
   if (fst == NULL)
      return -1;
   fst->fpn = iter;
   fst->fp_next = NULL;
   fst->owner = NULL;
   mp->free_fp_list = fst;

   /* We have list with first element, fill in the rest num-1 element member*/
   for (iter = 1; iter < numfp; iter++)
   {
      newfst = malloc(sizeof(struct framephy_struct));
      if (newfst == NULL) {
         struct framephy_struct *frame;
         while ((frame = mp->free_fp_list) != NULL) {
            mp->free_fp_list = frame->fp_next;
            free(frame);
         }
         return -1;
      }
      newfst->fpn = iter;
      newfst->fp_next = NULL;
      newfst->owner = NULL;
      fst->fp_next = newfst;
      fst = newfst;
   }

   return 0;
}

int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   struct framephy_struct *fp;
   if (mp == NULL || retfpn == NULL)
      return -1;
   pthread_mutex_lock(&free_frame_lock);
   if (mp->free_fp_list == NULL) {
      pthread_mutex_unlock(&free_frame_lock);
      return -1;
   }
   fp = mp->free_fp_list;
   mp->free_fp_list = fp->fp_next;
   *retfpn = fp->fpn;
   free(fp);
   pthread_mutex_unlock(&free_frame_lock);
   return 0;
}

int MEMPHY_dump(struct memphy_struct *mp)
{
  /*
   * TODO dump memphy contnt mp->storage
   *     for tracing the memory content
   */

   return 0;
}

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   struct framephy_struct *newnode;
   if (mp == NULL || mp->storage == NULL ||
       fpn >= (addr_t)mp->maxsz /
#ifdef MM64
       PAGING64_PAGESZ
#else
       PAGING_PAGESZ
#endif
       ) {
      return -1;
   }
   newnode = malloc(sizeof(struct framephy_struct));
   if (newnode == NULL)
      return -1;
   newnode->fpn = fpn;
   newnode->owner = NULL;
   pthread_mutex_lock(&free_frame_lock);
   newnode->fp_next = mp->free_fp_list;
   mp->free_fp_list = newnode;
   pthread_mutex_unlock(&free_frame_lock);
   return 0;
}

void destroy_memphy(struct memphy_struct *mp)
{
   struct framephy_struct *frame;
   if (mp == NULL) {
      return;
   }
   pthread_mutex_lock(&free_frame_lock);
   while ((frame = mp->free_fp_list) != NULL) {
      mp->free_fp_list = frame->fp_next;
      free(frame);
   }
   free(mp->storage);
   memset(mp, 0, sizeof(*mp));
   pthread_mutex_unlock(&free_frame_lock);
}

/*
 *  Init MEMPHY struct
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   if (mp == NULL)
      return -1;
   memset(mp, 0, sizeof(*mp));
   if (max_size == 0 || max_size > INT_MAX)
      return -1;
   mp->storage = (BYTE *)malloc(max_size * sizeof(BYTE));
   if (mp->storage == NULL)
      return -1;
   mp->maxsz = max_size;
   memset(mp->storage, 0, max_size * sizeof(BYTE));

#ifdef MM64
   if (MEMPHY_format(mp, PAGING64_PAGESZ) != 0)
#else
   if (MEMPHY_format(mp, PAGING_PAGESZ) != 0)
#endif
   {
      free(mp->storage);
      mp->storage = NULL;
      mp->maxsz = 0;
      return -1;
   }

   mp->rdmflg = (randomflg != 0) ? 1 : 0;

   if (!mp->rdmflg) /* Not Ramdom acess device, then it serial device*/
      mp->cursor = 0;

   return 0;
}

// #endif
